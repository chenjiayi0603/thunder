/*******************************************************************************
 * Project:  Net
 * @file     TcpCenterConnector.cpp
 * @brief    CenterConnector 的 TCP + Protobuf 实现
 * @author   cjy
 * @date:    2026年5月24日
 * @note
 *   从 Manager.cpp 迁移而来，独立管理到 Center 的 TCP 连接。
 *   协议格式：MsgHead (fixed header) + MsgBody (protobuf)
 *
 *   握手流程：
 *     Connector                           Center
 *       |-- connect() ----------------------->|
 *       |-- CMD_REQ_CONNECT_TO_WORKER ------->|
 *       |<---- CMD_RSP_CONNECT_TO_WORKER -----|
 *       |-- CMD_REQ_TELL_WORKER ------------->|  (boHandshakeDone = true)
 *       |-- CMD_REQ_NODE_REGISTER ----------->|
 *       |<---- CMD_RSP_NODE_REGISTER ---------|  (node_id, route snapshot, etc.)
 *       |-- CMD_REQ_NODE_STATUS_REPORT ------>|  (periodic heartbeat)
 ******************************************************************************/
#include "TcpCenterConnector.hpp"
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include "protocol/oss_sys.pb.h"
#include "util/CBuffer.hpp"
#include "util/CommonUtils.hpp"
#include "util/json/CJsonObject.hpp"
#include "labor/Labor.hpp"        // for gc_uiMsgHeadSize
#include "NetDefine.hpp"          // for CMD_* constants

#include <log4cplus/logger.h>

// 使用独立 logger category
#define TC_LOG_TRACE(logEvent) LOG4CPLUS_TRACE(GetLogger(), logEvent)
#define TC_LOG_DEBUG(logEvent) LOG4CPLUS_DEBUG(GetLogger(), logEvent)
#define TC_LOG_INFO(logEvent)  LOG4CPLUS_INFO(GetLogger(), logEvent)
#define TC_LOG_WARN(logEvent)  LOG4CPLUS_WARN(GetLogger(), logEvent)
#define TC_LOG_ERROR(logEvent) LOG4CPLUS_ERROR(GetLogger(), logEvent)

namespace net
{

namespace
{

// ---- 工具函数 ----

void Trim(std::string& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    s.erase(0, i);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
}

void SetNonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void SetSockOpts(int fd)
{
    int on = 1;
    setsockopt(fd, SOL_SOCKET,   SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP,  TCP_NODELAY,  &on, sizeof(on));
    int idle = 60, intvl = 5, cnt = 3;
    setsockopt(fd, SOL_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, SOL_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
}

log4cplus::Logger& GetLogger()
{
    static log4cplus::Logger logger = log4cplus::Logger::getInstance("TcpCenterConnector");
    return logger;
}

// ---- 常量 ----
static constexpr size_t kMsgHeadSize = gc_uiMsgHeadSize;  // MsgHead 固定头大小

} // anonymous namespace


// ========== 构造/析构 ==========

TcpCenterConnector::TcpCenterConnector(const util::CJsonObject& oCenterConf)
    : m_oCenterConf(oCenterConf)
{}

TcpCenterConnector::~TcpCenterConnector()
{
    Destroy();
}

// ========== 生命周期 ==========

bool TcpCenterConnector::Init(struct ev_loop* loop, CenterEventCallback cb, void* user_data)
{
    if (m_bInited) return true;

    m_loop      = loop;
    m_callback  = std::move(cb);
    m_user_data = user_data;

    // 解析 Center 地址
    auto insert = [this](const std::string& id) {
        if (m_mapCenterConn.find(id) == m_mapCenterConn.end())
        {
            m_mapCenterConn[id] = nullptr;  // 延迟连接
            TC_LOG_TRACE("Init: center " << id);
        }
    };

    if (m_oCenterConf.IsArray() && m_oCenterConf.GetArraySize() > 0)
    {
        for (int i = 0; i < m_oCenterConf.GetArraySize(); ++i)
        {
            std::string id = m_oCenterConf[i]("host") + std::string(":")
                           + m_oCenterConf[i]("port") + std::string(".0");
            insert(id);
        }
    }
    else
    {
        // 单个 host:port (JSON object, 兼容 port 为 int/string)
        std::string host;
        {
            std::string portStr;
            int portInt = 0;
            if (m_oCenterConf.Get("host", host) && !host.empty()
                && (m_oCenterConf.Get("port", portStr) || m_oCenterConf.Get("port", portInt)))
            {
                if (!portStr.empty())
                    insert(host + std::string(":") + portStr + std::string(".0"));
                else if (portInt > 0)
                    insert(host + std::string(":") + std::to_string(portInt) + std::string(".0"));
            }
        }
        // CSV 格式: CJsonObject 直接包裹 string 类型,或通过 "center" 键获取
        {
            std::string csv;
            if (!m_oCenterConf.GetAsString(csv))
                m_oCenterConf.Get("center", csv);

            if (!csv.empty())
            {
                if (csv.find(',') != std::string::npos)
                {
                    size_t start = 0;
                    while (start < csv.size())
                    {
                        size_t comma = csv.find(',', start);
                        std::string token = (comma == std::string::npos)
                            ? csv.substr(start) : csv.substr(start, comma - start);
                        start = (comma == std::string::npos) ? csv.size() : comma + 1;
                        Trim(token);
                        if (token.empty()) continue;
                        size_t colon = token.rfind(':');
                        if (colon != std::string::npos && colon + 1 < token.size())
                        {
                            std::string h = token.substr(0, colon);
                            std::string p = token.substr(colon + 1);
                            Trim(h); Trim(p);
                            if (!h.empty() && !p.empty())
                                insert(h + std::string(":") + p + std::string(".0"));
                        }
                    }
                }
                else
                {
                    // 单个 host:port 字符串
                    Trim(csv);
                    insert(csv + std::string(".0"));
                }
            }
        }
    }

    if (m_mapCenterConn.empty())
        TC_LOG_WARN("Init: no center configured");
    else
        TC_LOG_INFO("Init: " << m_mapCenterConn.size() << " center(s)");

    m_bInited = true;
    return true;
}

void TcpCenterConnector::Destroy()
{
    if (!m_bInited) return;

    while (!m_mapCenterConn.empty())
        DestroyConnByIter(m_mapCenterConn.begin());
    m_mapCenterConn.clear();
    m_strRaftLeaderKey.clear();

    m_loop      = nullptr;
    m_callback  = nullptr;
    m_user_data = nullptr;
    m_bInited   = false;
}

// ========== 节点信息 ==========

void TcpCenterConnector::SetNodeInfo(const std::string& nodeType,
                                      const std::string& hostForServer, int portForServer,
                                      const std::string& hostForClient, int portForClient,
                                      const std::string& gateway, int gatewayPort,
                                      unsigned int workerNum)
{
    m_nodeType      = nodeType;
    m_hostForServer = hostForServer;
    m_portForServer = portForServer;
    m_hostForClient = hostForClient;
    m_portForClient = portForClient;
    m_gateway       = gateway;
    m_gatewayPort   = gatewayPort;
    m_workerNum     = workerNum;
}

// ========== CenterConnector 接口 ==========

bool TcpCenterConnector::ReportNodeStatus(const std::string& node_report, bool is_register)
{
    if (m_mapCenterConn.empty())
    {
        TC_LOG_TRACE("ReportNodeStatus: no center configured");
        return false;
    }

    uint32_t cmd = is_register ? CMD_REQ_NODE_REGISTER : CMD_REQ_NODE_STATUS_REPORT;
    uint32_t seq = GetSeq();

    auto sendOne = [&](const std::string& key, CenterConnectionAttr* pConn) {
        if (!pConn || pConn->iFd <= 0)
        {
            AutoConnect(key, cmd, seq, node_report);
        }
        else if (pConn->boHandshakeDone)
        {
            SendRegOrBeat(pConn, cmd, seq, node_report);
        }
        else
        {
            // 握手未完成，缓冲
            MsgHead oHead;
            MsgBody oBody;
            oHead.set_cmd(cmd);
            oHead.set_seq(seq);
            oBody.set_body(node_report);
            oHead.set_msgbody_len(oBody.ByteSize());
            pConn->pWaitForSendBuff->Write(oHead.SerializeAsString().c_str(), oHead.ByteSize());
            pConn->pWaitForSendBuff->Write(oBody.SerializeAsString().c_str(), oBody.ByteSize());
            TC_LOG_TRACE("ReportNodeStatus: buffered for " << key);
        }
    };

    if (m_strRaftLeaderKey.empty())
    {
        for (auto& kv : m_mapCenterConn)
            sendOne(kv.first, kv.second.get());
    }
    else
    {
        auto it = m_mapCenterConn.find(m_strRaftLeaderKey);
        if (it != m_mapCenterConn.end())
            sendOne(it->first, it->second.get());
        else
        {
            TC_LOG_WARN("ReportNodeStatus: leader " << m_strRaftLeaderKey << " gone, fan-out");
            m_strRaftLeaderKey.clear();
            for (auto& kv : m_mapCenterConn)
                sendOne(kv.first, kv.second.get());
        }
    }

    return true;
}

bool TcpCenterConnector::IsConnected() const
{
    for (const auto& kv : m_mapCenterConn)
        if (kv.second && kv.second->iFd > 0 && kv.second->boHandshakeDone)
            return true;
    return false;
}

size_t TcpCenterConnector::CenterCount() const
{
    return m_mapCenterConn.size();
}

bool TcpCenterConnector::TryConsumeMessage(int iFd, uint32_t ulSeq,
                                            const std::string& strIdentify,
                                            uint32_t cmd, uint32_t seq, const std::string& body)
{
    auto it = m_mapCenterConn.find(strIdentify);
    if (it == m_mapCenterConn.end())
        return false;  // 非本插件管辖的 Center

    CenterConnectionAttr* pConn = it->second.get();

    // fd/seq 校验：仅在有活跃连接时生效；无连接时允许穿透以支持单测/脱管场景
    if (pConn && (pConn->iFd != iFd || pConn->ulSeq != ulSeq))
    {
        TC_LOG_TRACE("TryConsumeMessage: stale fd/seq for " << strIdentify);
        return true;  // 已消费（过期的包）
    }

    // ---- 协议分发 ----
    TC_LOG_TRACE("TryConsumeMessage: cmd " << cmd << " seq " << seq
                 << " from " << strIdentify);

    if (cmd == CMD_RSP_CONNECT_TO_WORKER)
    {
        // 握手完成（需要有效连接）
        if (pConn)
            OnHandshakeComplete(pConn);
    }
    else if (cmd == CMD_RSP_NODE_REGISTER || cmd == CMD_RSP_NODE_STATUS_REPORT)
    {
        NodeReportRsp oRsp;
        if (oRsp.ParseFromString(body))
        {
            // Raft leader 跟踪
            uint32_t err = oRsp.errcode();
            const std::string& leader = oRsp.current_leader_identify();

            if (err == 2u)
            {
                m_strRaftLeaderKey.clear();
                TC_LOG_TRACE("TryConsumeMessage: err=2, clear leader");
            }
            else if (!leader.empty())
            {
                if (m_mapCenterConn.find(leader) != m_mapCenterConn.end())
                {
                    if (m_strRaftLeaderKey != leader)
                    {
                        TC_LOG_INFO("TryConsumeMessage: raft leader -> " << leader
                                    << " (err=" << err << ")");
                    }
                    m_strRaftLeaderKey = leader;
                }
                else
                {
                    m_strRaftLeaderKey.clear();
                    TC_LOG_TRACE("TryConsumeMessage: leader " << leader << " not local");
                }
            }
            else if (err == 0u)
            {
                m_strRaftLeaderKey.clear();
            }

            CenterEvent ev;
            if (err == 0)
            {
                ev.type    = CenterEventType::Registered;
                ev.node_id = oRsp.node_id();
                ev.errcode = 0;

                // 路由快照
                const NodeNotice& snap = oRsp.subscribed_route_snapshot();
                if (snap.node_arry_reg_size() > 0)
                {
                    ev.route_snapshot = snap.SerializeAsString();
                }

                TC_LOG_INFO("TryConsumeMessage: registered, node_id=" << ev.node_id
                            << " route_nodes=" << snap.node_arry_reg_size());
            }
            else
            {
                ev.type    = CenterEventType::Registered;
                ev.errcode = static_cast<int>(err);
                ev.errmsg  = "center returned error code " + std::to_string(err);
                TC_LOG_WARN("TryConsumeMessage: err=" << err << " from " << strIdentify);
            }

            Emit(ev.type, ev);
        }
        else
        {
            TC_LOG_WARN("TryConsumeMessage: parse NodeReportRsp failed");
        }
    }
    else if (cmd == CMD_REQ_SET_NODE_CUSTOM_CONFIG)
    {
        // Center 推送的自定义配置
        ConfigInfo oCfg;
        if (oCfg.ParseFromString(body))
        {
            CenterEvent ev;
            ev.type          = CenterEventType::ConfigUpdated;
            ev.config_content = oCfg.file_content();
            Emit(CenterEventType::ConfigUpdated, ev);
            TC_LOG_INFO("TryConsumeMessage: config updated, bytes=" << ev.config_content.size());
        }
    }
    else if (cmd == CMD_REQ_NODE_STOP)
    {
        Emit(CenterEventType::NodeStop);
    }
    else if (cmd == CMD_REQ_NODE_RESTART_WORKERS)
    {
        Emit(CenterEventType::NodeRestartWorkers);
    }
    else
    {
        TC_LOG_TRACE("TryConsumeMessage: unhandled cmd " << cmd);
    }

    return true;
}

bool TcpCenterConnector::IsCenterConnection(const std::string& strIdentify) const
{
    return m_mapCenterConn.find(strIdentify) != m_mapCenterConn.end();
}

void TcpCenterConnector::OnConnectionDestroy(const std::string& strIdentify,
                                              int iFd, uint32_t ulSeq)
{
    auto it = m_mapCenterConn.find(strIdentify);
    if (it == m_mapCenterConn.end() || !it->second) return;

    CenterConnectionAttr* pConn = it->second.get();
    if (pConn->iFd != iFd || pConn->ulSeq != ulSeq) return;

    TC_LOG_INFO("OnConnectionDestroy: " << strIdentify << " fd " << iFd);
    ResetConnForReconnect(pConn);
}

// ========== 内部连接管理 ==========

CenterConnectionAttr* TcpCenterConnector::CreateConnAttr(int iFd, uint32_t ulSeq)
{
    auto p = std::make_unique<CenterConnectionAttr>();
    p->iFd          = iFd;
    p->ulSeq        = ulSeq;
    p->dActiveTime  = Now();
    p->pRecvBuff    = std::make_unique<util::CBuffer>();
    p->pSendBuff    = std::make_unique<util::CBuffer>();
    p->pWaitForSendBuff = std::make_unique<util::CBuffer>();
    p->pConnector   = this;
    return p.release();
}

void TcpCenterConnector::DestroyConn(const std::string& strIdentify)
{
    auto it = m_mapCenterConn.find(strIdentify);
    if (it != m_mapCenterConn.end())
        DestroyConnByIter(it);
}

void TcpCenterConnector::DestroyConnByIter(
    std::unordered_map<std::string, std::unique_ptr<CenterConnectionAttr>>::iterator iter)
{
    if (iter == m_mapCenterConn.end()) return;

    CenterConnectionAttr* p = iter->second.get();
    if (!p) { m_mapCenterConn.erase(iter); return; }

    TC_LOG_TRACE("DestroyConn: " << iter->first << " fd " << p->iFd);

    if (p->pIoWatcher)
    {
        ev_io_stop(m_loop, p->pIoWatcher);
        delete p->pIoWatcher;
        p->pIoWatcher = nullptr;
    }
    if (p->pTimeoutWatcher)
    {
        ev_timer_stop(m_loop, p->pTimeoutWatcher);
        delete p->pTimeoutWatcher;
        p->pTimeoutWatcher = nullptr;
    }
    if (p->iFd >= 0)
    {
        close(p->iFd);
        p->iFd = -1;
    }

    if (iter->first == m_strRaftLeaderKey)
        m_strRaftLeaderKey.clear();

    m_mapCenterConn.erase(iter);
}

void TcpCenterConnector::ResetConnForReconnect(CenterConnectionAttr* pConn)
{
    if (!pConn) return;

    // 停止 watcher（不 delete，后面可能重新使用）
    if (pConn->pIoWatcher)
    {
        ev_io_stop(m_loop, pConn->pIoWatcher);
        delete pConn->pIoWatcher;
        pConn->pIoWatcher = nullptr;
    }
    if (pConn->pTimeoutWatcher)
    {
        ev_timer_stop(m_loop, pConn->pTimeoutWatcher);
        delete pConn->pTimeoutWatcher;
        pConn->pTimeoutWatcher = nullptr;
    }

    // 关闭 fd
    if (pConn->iFd >= 0)
    {
        close(pConn->iFd);
        pConn->iFd = -1;
    }

    pConn->ulSeq           = 0;
    pConn->boHandshakeDone = false;
    pConn->boConnecting    = false;

    // 重建缓冲区（保留 pWaitForSendBuff 中可能缓存的数据？不，注册数据定期重发）
    pConn->pRecvBuff = std::make_unique<util::CBuffer>();
    pConn->pSendBuff = std::make_unique<util::CBuffer>();
    pConn->pWaitForSendBuff = std::make_unique<util::CBuffer>();

    if (pConn->strIdentify == m_strRaftLeaderKey)
        m_strRaftLeaderKey.clear();
}

// ========== IO 事件注册 ==========

bool TcpCenterConnector::AddIoRead(CenterConnectionAttr* pConn)
{
    if (!pConn->pIoWatcher)
    {
        auto* w = new ev_io();
        w->data = static_cast<void*>(pConn);
        ev_io_init(w, IoCallback, pConn->iFd, EV_READ);
        ev_io_start(m_loop, w);
        pConn->pIoWatcher = w;
    }
    else
    {
        ev_io_stop(m_loop, pConn->pIoWatcher);
        ev_io_set(pConn->pIoWatcher, pConn->iFd,
                  pConn->pIoWatcher->events | EV_READ);
        ev_io_start(m_loop, pConn->pIoWatcher);
    }
    return true;
}

bool TcpCenterConnector::AddIoWrite(CenterConnectionAttr* pConn)
{
    if (!pConn->pIoWatcher)
    {
        auto* w = new ev_io();
        w->data = static_cast<void*>(pConn);
        ev_io_init(w, IoCallback, pConn->iFd, EV_WRITE);
        ev_io_start(m_loop, w);
        pConn->pIoWatcher = w;
    }
    else
    {
        ev_io_stop(m_loop, pConn->pIoWatcher);
        ev_io_set(pConn->pIoWatcher, pConn->iFd,
                  pConn->pIoWatcher->events | EV_WRITE);
        ev_io_start(m_loop, pConn->pIoWatcher);
    }
    return true;
}

bool TcpCenterConnector::RemoveIoWrite(CenterConnectionAttr* pConn)
{
    if (!pConn->pIoWatcher) return true;

    if (pConn->pIoWatcher->events & EV_WRITE)
    {
        ev_io_stop(m_loop, pConn->pIoWatcher);
        if (pConn->pIoWatcher->events & EV_READ)
        {
            ev_io_set(pConn->pIoWatcher, pConn->iFd, EV_READ);
            ev_io_start(m_loop, pConn->pIoWatcher);
        }
    }
    return true;
}

bool TcpCenterConnector::AddIoTimeout(CenterConnectionAttr* pConn, double dTimeout)
{
    if (!pConn->pTimeoutWatcher)
    {
        auto* w = new ev_timer();
        w->data = static_cast<void*>(pConn);
        ev_timer_init(w, IoTimerCallback, dTimeout, 0.0);
        ev_timer_start(m_loop, w);
        pConn->pTimeoutWatcher = w;
    }
    else
    {
        ev_timer_stop(m_loop, pConn->pTimeoutWatcher);
        ev_timer_set(pConn->pTimeoutWatcher, dTimeout, 0.0);
        ev_timer_start(m_loop, pConn->pTimeoutWatcher);
    }
    return true;
}

// ========== 发送 ==========

bool TcpCenterConnector::SendRegOrBeat(CenterConnectionAttr* pConn,
                                        uint32_t cmd, uint32_t seq, const std::string& body)
{
    MsgHead oHead;
    MsgBody oBody;
    oHead.set_cmd(cmd);
    oHead.set_seq(seq);
    oBody.set_body(body);
    oHead.set_msgbody_len(oBody.ByteSize());

    pConn->pSendBuff->Write(oHead.SerializeAsString().c_str(), oHead.ByteSize());
    pConn->pSendBuff->Write(oBody.SerializeAsString().c_str(), oBody.ByteSize());

    return FlushSendBuf(pConn);
}

bool TcpCenterConnector::FlushSendBuf(CenterConnectionAttr* pConn)
{
    util::CBuffer* pBuf = pConn->pSendBuff.get();
    int iNeed = static_cast<int>(pBuf->ReadableBytes());
    if (iNeed == 0) return true;

    int iErrno = 0;
    int iWrote = pBuf->WriteFD(pConn->iFd, iErrno);

    TC_LOG_TRACE("FlushSendBuf: fd " << pConn->iFd << " wrote " << iWrote << "/" << iNeed);

    if (iWrote < 0)
    {
        if (EAGAIN != iErrno && EINTR != iErrno)
        {
            TC_LOG_ERROR("FlushSendBuf: fd " << pConn->iFd
                         << " error " << iErrno << ": " << strerror_r(iErrno, m_errBuf, kErrBufLen));
            DestroyConn(pConn->strIdentify);
            return false;
        }
        else if (EAGAIN == iErrno)
        {
            pConn->dActiveTime = Now();
            AddIoWrite(pConn);
        }
    }
    else if (iWrote > 0)
    {
        pConn->dActiveTime = Now();
        if (iWrote == iNeed)
            RemoveIoWrite(pConn);
        else
            AddIoWrite(pConn);
    }

    pBuf->Compact(32784);
    return true;
}

bool TcpCenterConnector::AutoConnect(const std::string& strIdentify,
                                      uint32_t cmd, uint32_t seq, const std::string& body)
{
    TC_LOG_TRACE("AutoConnect: " << strIdentify << " cmd " << cmd);

    // 解析 identify: host:port.worker_index
    size_t colon = strIdentify.find(':');
    if (colon == std::string::npos)
    {
        TC_LOG_ERROR("AutoConnect: invalid identify " << strIdentify);
        return false;
    }

    size_t dot = strIdentify.rfind('.');
    if (dot == std::string::npos || dot < colon)
        dot = std::string::npos;

    std::string strHost = strIdentify.substr(0, colon);
    std::string strPort = (dot != std::string::npos)
        ? strIdentify.substr(colon + 1, dot - (colon + 1))
        : strIdentify.substr(colon + 1);

    int iPort = atoi(strPort.c_str());
    if (iPort == 0)
    {
        TC_LOG_ERROR("AutoConnect: invalid port in " << strIdentify);
        return false;
    }

    struct sockaddr addr;
    int iFd = -1;
    if (!ResolveHostPort(strHost, iPort, addr, iFd))
    {
        TC_LOG_ERROR("AutoConnect: resolve failed for " << strHost << ":" << iPort);
        return false;
    }

    SetNonblock(iFd);
    SetSockOpts(iFd);

    uint32_t ulSeq = ++m_ulConnSeq > 0 ? m_ulConnSeq : ++m_ulConnSeq;

    auto pConn = std::make_unique<CenterConnectionAttr>();
    pConn->iFd          = iFd;
    pConn->ulSeq        = ulSeq;
    pConn->dActiveTime  = Now();
    pConn->pRecvBuff    = std::make_unique<util::CBuffer>();
    pConn->pSendBuff    = std::make_unique<util::CBuffer>();
    pConn->pWaitForSendBuff = std::make_unique<util::CBuffer>();
    pConn->strIdentify  = strIdentify;
    pConn->pConnector   = this;
    pConn->boConnecting = true;

    // 缓冲注册/心跳数据到 pWaitForSendBuff
    MsgHead oHead;
    MsgBody oBody;
    oHead.set_cmd(cmd);
    oHead.set_seq(seq);
    oBody.set_body(body);
    oHead.set_msgbody_len(oBody.ByteSize());
    pConn->pWaitForSendBuff->Write(oHead.SerializeAsString().c_str(), oHead.ByteSize());
    pConn->pWaitForSendBuff->Write(oBody.SerializeAsString().c_str(), oBody.ByteSize());

    // 发起 connect
    if (connect(iFd, &addr, sizeof(addr)) < 0 && errno != EINPROGRESS)
    {
        TC_LOG_ERROR("AutoConnect: connect " << strIdentify
                     << " error " << errno << ": " << strerror_r(errno, m_errBuf, kErrBufLen));
        close(iFd);
        return false;
    }

    // 注册 IO 事件
    CenterConnectionAttr* pRaw = pConn.get();
    m_mapCenterConn[strIdentify] = std::move(pConn);

    AddIoWrite(pRaw);   // 等待 connect 完成
    AddIoRead(pRaw);
    AddIoTimeout(pRaw, kConnTimeout);

    TC_LOG_INFO("AutoConnect: connecting to " << strIdentify << " fd " << iFd);
    return true;
}

// ========== 握手完成 ==========

void TcpCenterConnector::OnHandshakeComplete(CenterConnectionAttr* pConn)
{
    TC_LOG_TRACE("OnHandshakeComplete: " << pConn->strIdentify);

    // 发送 CMD_REQ_TELL_WORKER
    std::string workerIdentify = m_hostForServer + std::string(":") + std::to_string(m_portForServer);

    TargetWorker oTW;
    oTW.set_err_no(0);
    oTW.set_worker_identify(workerIdentify);
    oTW.set_node_type(m_nodeType);
    oTW.set_err_msg("OK");

    MsgHead oHead;
    MsgBody oBody;
    oHead.set_cmd(CMD_REQ_TELL_WORKER);
    oHead.set_seq(GetSeq());
    oBody.set_body(oTW.SerializeAsString());
    oHead.set_msgbody_len(oBody.ByteSize());

    pConn->pSendBuff->Write(oHead.SerializeAsString().c_str(), oHead.ByteSize());
    pConn->pSendBuff->Write(oBody.SerializeAsString().c_str(), oBody.ByteSize());

    // 倒出缓冲的数据
    if (pConn->pWaitForSendBuff->ReadableBytes() > 0)
    {
        pConn->pSendBuff->Write(pConn->pWaitForSendBuff->GetRawReadBuffer(),
                                pConn->pWaitForSendBuff->ReadableBytes());
        pConn->pWaitForSendBuff->SkipBytes(pConn->pWaitForSendBuff->ReadableBytes());
    }

    pConn->boHandshakeDone = true;
    pConn->boConnecting    = false;

    FlushSendBuf(pConn);
}

// ========== IO 回调（静态 → 成员） ==========

void TcpCenterConnector::IoCallback(struct ev_loop* loop, struct ev_io* watcher, int revents)
{
    (void)loop;
    if (!watcher->data) return;

    CenterConnectionAttr* pConn = static_cast<CenterConnectionAttr*>(watcher->data);
    TcpCenterConnector* self = pConn->pConnector;
    if (!self) return;

    if (revents & EV_READ)
        self->OnIoRead(pConn);
    if (revents & EV_WRITE)
        self->OnIoWrite(pConn);
    if (revents & EV_ERROR)
        self->OnIoError(pConn);
}

void TcpCenterConnector::IoTimerCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    (void)loop;
    (void)revents;
    if (!watcher->data) return;

    CenterConnectionAttr* pConn = static_cast<CenterConnectionAttr*>(watcher->data);
    TcpCenterConnector* self = pConn->pConnector;
    if (self) self->OnIoTimeout(pConn);
}

// ========== IO 处理 ==========

void TcpCenterConnector::OnIoRead(CenterConnectionAttr* pConn)
{
    int iErrno = 0;
    int iLen = pConn->pRecvBuff->ReadFD(pConn->iFd, iErrno);

    TC_LOG_TRACE("OnIoRead: fd " << pConn->iFd << " read " << iLen
                 << " bytes, buffered " << pConn->pRecvBuff->ReadableBytes());

    if (iLen > 0)
    {
        pConn->dActiveTime = Now();
        RecvAndDispatch(pConn);
    }
    else if (iLen == 0)
    {
        TC_LOG_TRACE("OnIoRead: fd " << pConn->iFd << " closed by peer");
        DestroyConn(pConn->strIdentify);

        CenterEvent ev;
        ev.type = CenterEventType::ConnectionLost;
        Emit(CenterEventType::ConnectionLost, ev);
    }
    else
    {
        if (EAGAIN != iErrno && EINTR != iErrno)
        {
            TC_LOG_ERROR("OnIoRead: fd " << pConn->iFd
                         << " error " << iErrno << ": " << strerror_r(iErrno, m_errBuf, kErrBufLen));
            DestroyConn(pConn->strIdentify);

            CenterEvent ev;
            ev.type = CenterEventType::ConnectionLost;
            Emit(CenterEventType::ConnectionLost, ev);
        }
    }
}

void TcpCenterConnector::OnIoWrite(CenterConnectionAttr* pConn)
{
    int iErrno = 0;
    int iNeed = static_cast<int>(pConn->pSendBuff->ReadableBytes());

    // 连接建立后的首次 write 事件：iNeed == 0 且 pWaitForSendBuff 有数据
    if (iNeed == 0 && pConn->boConnecting)
    {
        // connect 成功，发送 CMD_REQ_CONNECT_TO_WORKER
        TC_LOG_TRACE("OnIoWrite: connect complete for " << pConn->strIdentify);

        ConnectWorker oCW;
        oCW.set_worker_index(0);  // Center 只有一个 Worker

        MsgHead oHead;
        MsgBody oBody;
        oHead.set_cmd(CMD_REQ_CONNECT_TO_WORKER);
        oHead.set_seq(GetSeq());
        oBody.set_body(oCW.SerializeAsString());
        oHead.set_msgbody_len(oBody.ByteSize());

        pConn->pSendBuff->Write(oHead.SerializeAsString().c_str(), oHead.ByteSize());
        pConn->pSendBuff->Write(oBody.SerializeAsString().c_str(), oBody.ByteSize());

        pConn->dActiveTime = Now();
        FlushSendBuf(pConn);
        return;
    }

    int iWrote = pConn->pSendBuff->WriteFD(pConn->iFd, iErrno);

    TC_LOG_TRACE("OnIoWrite: fd " << pConn->iFd << " wrote " << iWrote << "/" << iNeed);

    if (iWrote < 0)
    {
        if (EAGAIN != iErrno && EINTR != iErrno)
        {
            TC_LOG_ERROR("OnIoWrite: fd " << pConn->iFd
                         << " error " << iErrno << ": " << strerror_r(iErrno, m_errBuf, kErrBufLen));
            DestroyConn(pConn->strIdentify);
        }
        else if (EAGAIN == iErrno)
        {
            pConn->dActiveTime = Now();
            AddIoWrite(pConn);
        }
    }
    else if (iWrote > 0)
    {
        pConn->dActiveTime = Now();
        if (iWrote == iNeed)
            RemoveIoWrite(pConn);
        else
            AddIoWrite(pConn);
    }

    pConn->pSendBuff->Compact(32784);
}

void TcpCenterConnector::OnIoError(CenterConnectionAttr* pConn)
{
    TC_LOG_WARN("OnIoError: fd " << pConn->iFd << " " << pConn->strIdentify);
    DestroyConn(pConn->strIdentify);

    CenterEvent ev;
    ev.type = CenterEventType::ConnectionLost;
    Emit(CenterEventType::ConnectionLost, ev);
}

void TcpCenterConnector::OnIoTimeout(CenterConnectionAttr* pConn)
{
    double after = pConn->dActiveTime - Now() + kIoKeepAlive;
    if (after > 0)
    {
        // 有活动，刷新定时器
        TC_LOG_TRACE("OnIoTimeout: refresh " << pConn->strIdentify << " after " << after);
        AddIoTimeout(pConn, after);
        return;
    }

    TC_LOG_WARN("OnIoTimeout: " << pConn->strIdentify << " fd " << pConn->iFd);
    DestroyConn(pConn->strIdentify);

    CenterEvent ev;
    ev.type = CenterEventType::ConnectionLost;
    Emit(CenterEventType::ConnectionLost, ev);
}

// ========== 数据解析 ==========

void TcpCenterConnector::RecvAndDispatch(CenterConnectionAttr* pConn)
{
    while (pConn->pRecvBuff->ReadableBytes() >= kMsgHeadSize)
    {
        MsgHead oHead;
        if (!oHead.ParseFromArray(pConn->pRecvBuff->GetRawReadBuffer(), kMsgHeadSize))
        {
            TC_LOG_ERROR("RecvAndDispatch: parse MsgHead failed, fd " << pConn->iFd);
            DestroyConn(pConn->strIdentify);
            return;
        }

        uint32_t bodyLen = oHead.msgbody_len();
        if (pConn->pRecvBuff->ReadableBytes() < kMsgHeadSize + bodyLen)
            break;  // body 不完整，等待更多数据

        MsgBody oBody;
        if (bodyLen > 0)
        {
            if (!oBody.ParseFromArray(pConn->pRecvBuff->GetRawReadBuffer() + kMsgHeadSize, bodyLen))
            {
                TC_LOG_ERROR("RecvAndDispatch: parse MsgBody failed, fd " << pConn->iFd);
                DestroyConn(pConn->strIdentify);
                return;
            }
        }

        pConn->dActiveTime = Now();

        // 消费消息
        TryConsumeMessage(pConn->iFd, pConn->ulSeq, pConn->strIdentify,
                          oHead.cmd(), oHead.seq(), oBody.body());

        // 跳过已处理数据
        pConn->pRecvBuff->SkipBytes(kMsgHeadSize + bodyLen);
        pConn->pRecvBuff->Compact(32784);
    }
}

// ========== 事件发射 ==========

void TcpCenterConnector::Emit(CenterEventType type, CenterEvent ev)
{
    ev.type = type;
    if (m_callback)
        m_callback(ev);
}

// ========== 辅助函数 ==========

uint32_t TcpCenterConnector::GetSeq()
{
    return util::GetMsgSeq_MS();
}

bool TcpCenterConnector::ResolveHostPort(const std::string& strHost, int iPort,
                                          struct sockaddr& addr, int& iFd)
{
    struct addrinfo hints;
    struct addrinfo* result = nullptr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string sPort = std::to_string(iPort);
    int iRet = getaddrinfo(strHost.c_str(), sPort.c_str(), &hints, &result);
    if (iRet != 0 || !result)
    {
        TC_LOG_ERROR("ResolveHostPort: getaddrinfo " << strHost << ":" << iPort
                     << " error: " << gai_strerror(iRet));
        return false;
    }

    memcpy(&addr, result->ai_addr, result->ai_addrlen);
    iFd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    freeaddrinfo(result);

    if (iFd < 0)
    {
        TC_LOG_ERROR("ResolveHostPort: socket() error " << errno << ": "
                     << strerror_r(errno, m_errBuf, kErrBufLen));
        return false;
    }

    return true;
}

double TcpCenterConnector::Now() const
{
    return m_loop ? ev_now(m_loop) : 0.0;
}

} /* namespace net */
