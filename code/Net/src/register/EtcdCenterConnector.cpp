/*******************************************************************************
 * Project:  Net
 * @file     EtcdCenterConnector.cpp
 * @brief    CenterConnector 的 etcd 实现（Phase 1：注册 + node_id 分配）
 * @author   cjy
 * @date:    2026年6月3日
 * @note
 *   依赖：
 *     - util::CurlClient::PostHttps  — 同步 HTTP POST
 *     - base64.h (Apache base64)     — etcd gateway key/value 编解码
 *     - oss_sys.pb.h (NodeReport)    — 解析注册参数
 *     - CJsonObject                  — JSON 构建与解析
 *     - libev (ev_timer)             — KeepAlive 定时器（3s 周期）
 *
 *   注意：etcd HTTP v3 gateway 中数字类型（ID、count、TTL）均以字符串返回，
 *         需要 std::stoll / std::stoi 转换。
 ******************************************************************************/
#include "EtcdCenterConnector.hpp"
#include "register/EtcdHttpConn.hpp"
#include "register/EtcdParse.hpp"
#include "register/EtcdWatcher.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "protocol/oss_sys.pb.h"
#include "util/json/CJsonObject.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// ---- 日志宏 ----
// 使用实例成员 m_logger(由 Manager 经 SetLogger 注入本节点 logger),
// 故所有 ETCD_LOG_* 必须在成员函数内调用。issus #12: 原先硬编码 "Logic_robot"
// category 在非 Logic 节点上无 appender → etcd 日志全丢。
#define ETCD_LOG_DEBUG(msg) LOG4CPLUS_DEBUG(m_logger, msg)
#define ETCD_LOG_INFO(msg)  LOG4CPLUS_INFO(m_logger,  msg)
#define ETCD_LOG_WARN(msg)  LOG4CPLUS_WARN(m_logger,  msg)
#define ETCD_LOG_ERROR(msg) LOG4CPLUS_ERROR(m_logger, msg)

// ---- etcd key 命名空间 ----
static constexpr const char*    kSlotPrefix        = "/thunder/slot/";
static constexpr const char*    kRegistryPrefix    = "/thunder/registry/";
static constexpr uint32_t       kMaxSlot           = 255;
static constexpr int            kLeaseTTL          = 10;   ///< 秒
static constexpr double         kKeepAliveInterval = 3.0;  ///< 秒
/// watch 退化为快照 resync 轮询时的间隔（issus #20，bundled curl 不挂住 chunked 流）。
/// 路由变更在此间隔内传播；取 2s 兼顾新鲜度与降低无谓重连churn。
static constexpr int            kWatchResyncIntervalSec = 2;

namespace net
{

// ============================================================
// 构造 / 析构
// ============================================================

EtcdCenterConnector::EtcdCenterConnector(const util::CJsonObject& conf)
    : m_oConf(conf)
{
    // 仅保存配置，其余在 Init() 中初始化
}

EtcdCenterConnector::~EtcdCenterConnector()
{
    // 确保析构时资源已释放（正常路径由 Destroy() 处理）
    if (m_keepAliveTimerStarted && m_loop)
    {
        ev_timer_stop(m_loop, &m_keepAliveTimer);
        m_keepAliveTimerStarted = false;
    }
}

// ============================================================
// Init
// ============================================================

bool EtcdCenterConnector::Init(struct ev_loop* loop,
                                CenterEventCallback cb,
                                void* user_data)
{
    m_loop      = loop;
    m_callback  = cb;
    m_user_data = user_data;

    // 解析 etcd_endpoints（多节点逗号分隔，取第一个）
    std::string endpoints;
    m_oConf.Get("etcd_endpoints", endpoints);
    if (endpoints.empty())
    {
        ETCD_LOG_ERROR("EtcdCenterConnector::Init — etcd_endpoints 未配置");
        return false;
    }
    // 取第一个端点
    auto pos = endpoints.find(',');
    m_endpoint = (pos != std::string::npos) ? endpoints.substr(0, pos) : endpoints;
    // 去除首尾空白
    while (!m_endpoint.empty() && m_endpoint.front() == ' ') m_endpoint.erase(0, 1);
    while (!m_endpoint.empty() && m_endpoint.back()  == ' ') m_endpoint.pop_back();

    ETCD_LOG_INFO("EtcdCenterConnector::Init — endpoint=" << m_endpoint);

    // 解析 host:port, 创建异步 HTTP 连接(在 m_loop 上, 用框架 HttpCodec)
    {
        auto protoEnd = m_endpoint.find("://");
        std::string hostPort = (protoEnd != std::string::npos)
                                   ? m_endpoint.substr(protoEnd + 3) : m_endpoint;
        auto colon = hostPort.rfind(':');
        if (colon == std::string::npos)
        {
            ETCD_LOG_ERROR("EtcdCenterConnector::Init — 端点格式错误: " << m_endpoint);
            return false;
        }
        std::string host = hostPort.substr(0, colon);
        int port = 0;
        try { port = std::stoi(hostPort.substr(colon + 1)); } catch (...) {}
        if (port <= 0)
        {
            ETCD_LOG_ERROR("EtcdCenterConnector::Init — 端口解析失败: " << m_endpoint);
            return false;
        }
        m_http = std::make_unique<EtcdHttpConn>(m_loop, host, port, m_logger);
    }

    // 租约申请改为 fire-and-forget: queue 异步请求, 不阻塞 Init。
    // 若 etcd 即时可达则秒级拿到 lease; 若不可达, OnKeepAliveTimer 会补领。
    AsyncLeaseGrant([this](bool ok, int64_t leaseId) {
        if (ok) m_leaseId = leaseId;
    });

    // 启动 KeepAlive 定时器（每 kKeepAliveInterval 秒续租，值成员无需 new）
    m_keepAliveTimer.data = static_cast<void*>(this);
    ev_timer_init(&m_keepAliveTimer, KeepAliveTimerCallback,
                  kKeepAliveInterval, kKeepAliveInterval);
    ev_timer_start(m_loop, &m_keepAliveTimer);
    m_keepAliveTimerStarted = true;

    // Phase C: 启动 EtcdWatcher(主循环) + 初始 snapshot 取 revision
    StartWatch();

    ETCD_LOG_INFO("EtcdCenterConnector::Init — 成功");
    return true;
}

// ============================================================
// Destroy
// ============================================================

void EtcdCenterConnector::Destroy()
{
    // 停止定时器
    if (m_keepAliveTimerStarted && m_loop)
    {
        ev_timer_stop(m_loop, &m_keepAliveTimer);
        m_keepAliveTimerStarted = false;
    }

    // 停止 watcher(Phase C)
    StopWatch();

    // 撤销租约 fire-and-forget（不阻塞;lease 自身 TTL 到期也会清理）
    if (m_leaseId != 0)
    {
        m_http->Post("/v3/lease/revoke",
                      R"({"ID":")" + std::to_string(m_leaseId) + R"("})",
                      [](bool, int, const std::string&){});
        m_leaseId    = 0;
        m_registered = false;
    }

    m_http->Close();
    ETCD_LOG_INFO("EtcdCenterConnector::Destroy — 完成");
}

// ============================================================
// ReportNodeStatus
// ============================================================

bool EtcdCenterConnector::ReportNodeStatus(const std::string& node_report,
                                            bool is_register)
{
    // 解析 NodeReport protobuf（全局命名空间，无 oss:: 前缀）
    NodeReport report;
    if (!report.ParseFromString(node_report))
    {
        ETCD_LOG_ERROR("EtcdCenterConnector::ReportNodeStatus — NodeReport 解析失败");
        return false;
    }

    const std::string& ip   = report.node_ip();
    const uint32_t     port = report.node_port();
    const std::string& type = report.node_type();

    if (ip.empty() || port == 0)
    {
        ETCD_LOG_ERROR("EtcdCenterConnector::ReportNodeStatus — node_ip/node_port 为空");
        return false;
    }

    // 保存节点信息供 KeepAlive 定时器使用
    m_nodeIp   = ip;
    m_nodePort = port;
    m_nodeType = type;
    m_workerNum = report.worker_num();  // 写入 registry value, 供对端建路由 identify

    if (is_register || !m_registered)
    {
        // 注册流程（首次或显式注册）：槽位占位 or 幂等续期
        DoRegister(ip, port, type);
    }
    else
    {
        // 心跳：已注册且 lease 有效 → 异步续租, 结果由定时器汇总
        if (m_registered && m_leaseId != 0)
        {
            AsyncKeepAlive([](bool){});
        }
    }

    return true;
}

// ============================================================
// TryConsumeMessage / IsCenterConnection / OnConnectionDestroy
// ============================================================

bool EtcdCenterConnector::TryConsumeMessage(int /*iFd*/,
                                             uint32_t /*ulSeq*/,
                                             const std::string& /*strIdentify*/,
                                             uint32_t /*cmd*/,
                                             uint32_t /*seq*/,
                                             const std::string& /*body*/)
{
    // etcd 后端不走 TCP 消息通道
    return false;
}

bool EtcdCenterConnector::IsCenterConnection(const std::string& /*strIdentify*/) const
{
    return false;
}

void EtcdCenterConnector::OnConnectionDestroy(const std::string& /*strIdentify*/,
                                               int /*iFd*/,
                                               uint32_t /*ulSeq*/)
{
    // etcd 后端无 TCP 连接，空操作
}

// ============================================================
// etcd HTTP 基础操作
// ============================================================


void EtcdCenterConnector::AsyncLeaseGrant(std::function<void(bool, int64_t)> cb)
{
    util::CJsonObject oReq;
    oReq.Add("TTL", static_cast<int32_t>(kLeaseTTL));
    m_http->Post("/v3/lease/grant", oReq.ToString(),
        [this, cb = std::move(cb)](bool ok, int, const std::string& resp)
        {
            if (!ok || resp.empty()) { ETCD_LOG_ERROR("AsyncLeaseGrant — 请求失败"); cb(false, 0); return; }
            util::CJsonObject o;
            std::string idStr;
            if (!o.Parse(resp) || !o.Get("ID", idStr) || idStr.empty())
            { ETCD_LOG_ERROR("AsyncLeaseGrant — 解析失败: " << resp); cb(false, 0); return; }
            try { int64_t id = std::stoll(idStr); ETCD_LOG_INFO("AsyncLeaseGrant — ok leaseId=" << id); cb(true, id); }
            catch (...) { cb(false, 0); }
        });
}

void EtcdCenterConnector::AsyncKeepAlive(std::function<void(bool)> cb)
{
    if (m_leaseId == 0) { if (cb) cb(false); return; }
    m_http->Post("/v3/lease/keepalive", R"({"ID":")" + std::to_string(m_leaseId) + R"("})",
        [this, cb = std::move(cb)](bool ok, int, const std::string& resp)
        {
            util::CJsonObject o, r;
            int64_t ttl = 0;
            if (!ok || resp.empty() || !o.Parse(resp) || !o.Get("result", r)
                || !etcd_parse::GetGatewayInt64(r, "TTL", ttl) || ttl <= 0)
            { ETCD_LOG_WARN("AsyncKeepAlive — 失败 leaseId=" << m_leaseId); ++m_metricKeepAliveFail; if (cb) cb(false); return; }
            ++m_metricKeepAliveOk;
            ETCD_LOG_DEBUG("AsyncKeepAlive — ok leaseId=" << m_leaseId << " ttl=" << ttl);
            if (cb) cb(true);
        });
}

void EtcdCenterConnector::LeaseRevoke()
{
    if (m_leaseId == 0) return;
    m_http->Post("/v3/lease/revoke", R"({"ID":")" + std::to_string(m_leaseId) + R"("})",
        [this](bool, int, const std::string&){ /* fire-and-forget */ });
}

// ============================================================
// 异步 etcd 操作原语 + 延续链注册 (Phase B)
// ============================================================

void EtcdCenterConnector::AsyncQueryRegistry(const std::string& registryKey,
    std::function<void(bool, uint32_t, int64_t)> cb)
{
    util::CJsonObject oReq;
    oReq.Add("key", B64(registryKey));
    m_http->Post("/v3/kv/range", oReq.ToString(),
        [this, registryKey, cb = std::move(cb)](bool ok, int, const std::string& resp)
        {
            if (!ok || resp.empty()) { cb(false, 0, 0); return; }
            util::CJsonObject o;
            if (!o.Parse(resp)) { cb(false, 0, 0); return; }
            int64_t count = 0;
            if (!etcd_parse::GetGatewayInt64(o, "count", count) || count <= 0) { cb(false, 0, 0); return; }
            util::CJsonObject kvs, kv;
            if (!o.Get("kvs", kvs) || !kvs.IsArray() || !kvs.Get(0, kv)) { cb(false, 0, 0); return; }
            int64_t lease = 0; etcd_parse::GetGatewayInt64(kv, "lease", lease);
            std::string vB64; uint32_t nid = 0;
            if (kv.Get("value", vB64))
            {
                const std::string valueStr = B64Dec(vB64);
                util::CJsonObject val;
                if (val.Parse(valueStr))
                {
                    int64_t id = 0;
                    if (etcd_parse::GetGatewayInt64(val, "node_id", id) && id > 0 && id <= kMaxSlot)
                        nid = static_cast<uint32_t>(id);
                }
                if (nid == 0) { try { nid = static_cast<uint32_t>(std::stoul(valueStr)); } catch (...) {} }
            }
            cb(true, nid, lease);
        });
}

// registry value 的统一构造（slot 抢占与重绑共用，避免格式漂移；watch 依赖此格式解析）。
static std::string BuildRegistryValueJson(int nodeId, const std::string& ipPort,
                                          const std::string& nodeType, uint32_t workerNum)
{
    const auto colon = ipPort.rfind(':');
    return "{\"node_id\":" + std::to_string(nodeId) +
        ",\"node_type\":\"" + nodeType + "\"" +
        ",\"node_ip\":\"" + ipPort.substr(0, colon) + "\"" +
        ",\"node_port\":" + ipPort.substr(colon + 1) +
        ",\"worker_num\":" + std::to_string(workerNum > 0 ? workerNum : 1) + "}";
}

std::string EtcdCenterConnector::BuildSlotTxn(int slot,
                                               const std::string& slotKey,
                                               const std::string& registryKey,
                                               const std::string& ipPort,
                                               const std::string& nodeType)
{
    util::CJsonObject oTxn;

    // compare: slot/i 的 create_revision == 0（key 不存在）
    util::CJsonObject oCompare;
    util::CJsonObject oCmp;
    oCmp.Add("key",             B64(slotKey));
    oCmp.Add("target",          std::string("CREATE"));
    oCmp.Add("result",          std::string("EQUAL"));
    oCmp.Add("create_revision", std::string("0"));
    oCompare.Add(oCmp);
    oTxn.Add("compare", oCompare);

    // success: put slot/i = ip:port，put registry/ip:port = JSON{node_id, node_type, ip, port}
    util::CJsonObject oSuccess;

    util::CJsonObject oPutSlot;
    util::CJsonObject oPutSlotKV;
    oPutSlotKV.Add("key",   B64(slotKey));
    oPutSlotKV.Add("value", B64(ipPort));
    oPutSlotKV.Add("lease", std::to_string(m_leaseId));
    oPutSlot.Add("request_put", oPutSlotKV);
    oSuccess.Add(oPutSlot);

    // registry value: JSON with node_id/node_type/ip/port（供 watch 组装完整 NodeReport）
    std::string regValue = BuildRegistryValueJson(slot, ipPort, nodeType, m_workerNum);

    util::CJsonObject oPutReg;
    util::CJsonObject oPutRegKV;
    oPutRegKV.Add("key",   B64(registryKey));
    oPutRegKV.Add("value", B64(regValue));
    oPutRegKV.Add("lease", std::to_string(m_leaseId));
    oPutReg.Add("request_put", oPutRegKV);
    oSuccess.Add(oPutReg);

    oTxn.Add("success", oSuccess);

    // failure: range slot/i（满足协议格式，结果不使用）
    util::CJsonObject oFailure;
    util::CJsonObject oRangeFail;
    util::CJsonObject oRangeFailKV;
    oRangeFailKV.Add("key", B64(slotKey));
    oRangeFail.Add("request_range", oRangeFailKV);
    oFailure.Add(oRangeFail);
    oTxn.Add("failure", oFailure);

    return oTxn.ToString();
}

void EtcdCenterConnector::AsyncTryClaimSlot(int slot, const std::string& slotKey,
    const std::string& registryKey, const std::string& ipPort, const std::string& nodeType,
    std::function<void(bool)> cb)
{
    const std::string txnBody = BuildSlotTxn(slot, slotKey, registryKey, ipPort, nodeType);
    m_http->Post("/v3/kv/txn", txnBody,
        [this, slot, cb = std::move(cb)](bool ok, int, const std::string& resp)
        {
            if (!ok || resp.empty()) { ETCD_LOG_WARN("AsyncTryClaimSlot — HTTP 失败 slot=" << slot); cb(false); return; }
            util::CJsonObject o; bool succ = false;
            if (o.Parse(resp)) o.Get("succeeded", succ);
            cb(succ);
        });
}

// ============================================================
// 异步注册延续链 (issus #24 Phase B)
// DoRegister → OnRegEnsureLease → OnRegQuery → Fresh/Rebind/OnRegScan → OnRegDone
// ============================================================

void EtcdCenterConnector::DoRegister(const std::string& ip, uint32_t port, const std::string& nodeType)
{
    // #27: 异步链异常 m_regInProgress 永久 true 兜底: 30s 超时自动复位
    if (m_regInProgress) {
        if (++m_regStuckTicks > 10) {  // ~30s (keepalive timer 每 3s)
            ETCD_LOG_WARN("DoRegister — 注册卡住 >30s, 强制复位");
            m_regInProgress = false; m_regStuckTicks = 0;
        }
        return;
    }
    m_regStuckTicks  = 0;
    m_regInProgress = true;
    m_regSlot       = 0;
    // 参数已在 ReportNodeStatus 存入 m_nodeIp/m_nodePort/m_nodeType, 此处仅防遗漏
    if (m_nodeIp.empty()) { m_nodeIp = ip; m_nodePort = port; m_nodeType = nodeType; }
    OnRegEnsureLease();
}

void EtcdCenterConnector::OnRegEnsureLease()
{
    if (m_leaseId != 0) { OnRegQuery(); return; }
    ETCD_LOG_INFO("OnRegEnsureLease — 无 lease, 补领...");
    AsyncLeaseGrant([this](bool ok, int64_t id) {
        if (!ok) { OnRegDone(false, "lease grant 失败"); return; }
        m_leaseId = id;
        OnRegQuery();
    });
}

void EtcdCenterConnector::OnRegQuery()
{
    const std::string ipPort      = m_nodeIp + ":" + std::to_string(m_nodePort);
    const std::string registryKey = BuildRegistryPrefix(m_nodeType) + ipPort;
    ETCD_LOG_DEBUG("OnRegQuery — key=" << registryKey);
    AsyncQueryRegistry(registryKey, [this, ipPort, registryKey](bool found, uint32_t nid, int64_t lease) {
        const auto action = etcd_parse::DecideRegAction(found, lease, m_leaseId);
        if (action == etcd_parse::RegAction::Fresh)
        {
            m_nodeId = nid;
            // #29: Fresh 路径续租失败不阻塞注册——注册键已存在且 lease 匹配,
            // 续租是"添花"(确保 TTL 新鲜),失败不影响"已注册"这一事实。
            // 定时器 OnKeepAliveTimer 会在 ~3s 后再次续租, 连续失败自有
            // ConnectionLost 路径处理。
            AsyncKeepAlive([this, nid](bool ok) {
                (void)ok;
                m_registered = true;
                OnRegDone(true, "");
            });
        }
        else if (action == etcd_parse::RegAction::Rebind)
        {
            ETCD_LOG_INFO("OnRegQuery — 旧租约残留(lease=" << lease << "), 重绑 nid=" << nid);
            m_nodeId = nid;
            AsyncRebindRegistration(nid, registryKey, ipPort, m_nodeType,
                [this](bool ok) {
                    if (ok) { ++m_metricRegistryRebind; m_registered = true; OnRegDone(true, ""); }
                    else { ETCD_LOG_WARN("OnRegQuery — 重绑失败, 回退槽位抢占"); OnRegScan(); }
                });
        }
        else { OnRegScan(); }
    });
}

void EtcdCenterConnector::OnRegScan()
{
    const std::string ipPort = m_nodeIp + ":" + std::to_string(m_nodePort);
    // 计算起始槽位(首次进入)
    if (m_regSlot == 0)
    {
        uint32_t hashVal = 0;
        for (unsigned char c : ipPort) hashVal += c;
        m_regSlot = static_cast<int>(hashVal % kMaxSlot) + 1;
    }
    if (m_regSlot > static_cast<int>(kMaxSlot))
    {
        OnRegDone(false, "所有槽位已满(max=" + std::to_string(kMaxSlot) + ")");
        return;
    }
    const int         slotIdx = m_regSlot;
    const std::string slotKey = std::string(kSlotPrefix) + std::to_string(slotIdx);
    const std::string regKey  = BuildRegistryPrefix(m_nodeType) + ipPort;
    ETCD_LOG_DEBUG("OnRegScan — slot=" << slotIdx);
    AsyncTryClaimSlot(slotIdx, slotKey, regKey, ipPort, m_nodeType,
        [this, slotIdx, slotKey, regKey](bool ok) {
            if (ok) { m_nodeId = static_cast<uint32_t>(slotIdx); m_registered = true; OnRegDone(true, ""); }
            else { ++m_regSlot; OnRegScan(); }
        });
}

void EtcdCenterConnector::OnRegDone(bool ok, const std::string& errmsg)
{
    m_regInProgress  = false;
    m_regStuckTicks  = 0;
    CenterEvent ev;
    if (ok)
    {
        ev.node_id = m_nodeId;
        ETCD_LOG_INFO("<<< node_id 分配完成: " << m_nodeId << " (type=" << m_nodeType
                      << " addr=" << m_nodeIp << ":" << m_nodePort << " lease=" << m_leaseId << ") >>>");
    }
    else
    {
        ev.errcode = 1; ev.errmsg = errmsg;
        ETCD_LOG_ERROR("OnRegDone — 注册失败: " << errmsg);
    }
    Emit(CenterEventType::Registered, std::move(ev));
}

void EtcdCenterConnector::AsyncRebindRegistration(uint32_t nodeId,
                                                   const std::string& registryKey,
                                                   const std::string& ipPort,
                                                   const std::string& nodeType,
                                                   std::function<void(bool)> cb)
{
    if (m_leaseId == 0)
    {
        ETCD_LOG_ERROR("AsyncRebindRegistration — 当前无 lease");
        if (cb) cb(false); return;
    }
    const std::string slotKey = std::string(kSlotPrefix) + std::to_string(nodeId);
    const std::string regValue = BuildRegistryValueJson(static_cast<int>(nodeId), ipPort,
                                                        nodeType, m_workerNum);
    // 串行 PUT slot → PUT registry
    auto putKv = [&](const std::string& key, const std::string& val,
                     std::function<void(bool)> next) {
        util::CJsonObject o;
        o.Add("key",   B64(key));
        o.Add("value", B64(val));
        o.Add("lease", std::to_string(m_leaseId));
        m_http->Post("/v3/kv/put", o.ToString(),
            [key, next = std::move(next)](bool ok, int, const std::string&) {
                if (next) next(ok);
            });
    };
    // issue: [&] 捕获 regValue/registryKey 引用 → async 回调时栈已销毁 → dangling ref crash
    const int            nid     = static_cast<int>(nodeId);
    const std::string    leaseS  = std::to_string(m_leaseId);
    putKv(slotKey, ipPort,
        [this, registryKey, regValue, nid, leaseS, cb = std::move(cb)](bool ok1) {
        if (!ok1) { cb(false); return; }
        util::CJsonObject o;
        o.Add("key",   B64(registryKey));
        o.Add("value", B64(regValue));
        o.Add("lease", leaseS);
        m_http->Post("/v3/kv/put", o.ToString(),
            [cb = std::move(cb), nid, this](bool ok, int, const std::string&) {
            if (ok) ETCD_LOG_INFO("AsyncRebind — ok nodeId=" << nid << " lease=" << m_leaseId);
            else    ETCD_LOG_ERROR("AsyncRebind — registry PUT 失败");
            cb(ok);
        });
    });
}

void EtcdCenterConnector::SelfAuditRegistry()
{
    if (!m_registered || m_leaseId == 0 || m_nodeIp.empty() || m_regInProgress) return;
    const std::string ipPort      = m_nodeIp + ":" + std::to_string(m_nodePort);
    const std::string registryKey = BuildRegistryPrefix(m_nodeType) + ipPort;
    // fire-and-forget: 异步查 + 需要时异步重绑
    AsyncQueryRegistry(registryKey, [this, registryKey, ipPort](bool found, uint32_t nid, int64_t lease) {
        if (found && lease == m_leaseId) return;  // 健康
        ++m_metricSelfAuditFail;
        ETCD_LOG_ERROR("SelfAudit — 注册键异常(found=" << found << " lease=" << lease << "),立即自愈");
        const uint32_t useNodeId = found && nid > 0 ? nid : m_nodeId;
        AsyncRebindRegistration(useNodeId, registryKey, ipPort, m_nodeType,
            [this](bool ok) { if (ok) ++m_metricRegistryRebind; });
    });
}

// ============================================================
// KeepAlive 定时器
// ============================================================

/*static*/
void EtcdCenterConnector::KeepAliveTimerCallback(struct ev_loop* /*loop*/,
                                                   struct ev_timer* w,
                                                   int /*revents*/)
{
    if (!w || !w->data) return;
    auto* self = static_cast<EtcdCenterConnector*>(w->data);
    self->OnKeepAliveTimer();
}

void EtcdCenterConnector::OnKeepAliveTimer()
{
    if (m_regInProgress) return;  // 注册进行中,本轮跳过(避免 re-entrancy 竞态)

    if (m_leaseId == 0)
    {
        if (m_nodeIp.empty()) return;
        AsyncLeaseGrant([this](bool ok, int64_t id) {
            if (ok) { m_leaseId = id; DoRegister(m_nodeIp, m_nodePort, m_nodeType); }
        });
        return;
    }

    if (!m_registered)
    {
        AsyncKeepAlive([this](bool ok) {
            if (ok) { m_registered = true; m_keepAliveFailCount = 0; Emit(CenterEventType::ConnectionRestored); }
            else {
                ++m_keepAliveFailCount;
                if (m_keepAliveFailCount >= 10)
                {
                    LeaseRevoke(); m_leaseId = 0; m_keepAliveFailCount = 0;
                    DoRegister(m_nodeIp, m_nodePort, m_nodeType);
                }
            }
        });
        return;
    }

    AsyncKeepAlive([this](bool ok) {
        if (ok) { m_keepAliveFailCount = 0; }
        else {
            ETCD_LOG_WARN("OnKeepAliveTimer — 续租失败, ConnectionLost");
            m_registered = false; m_keepAliveFailCount = 1;
            Emit(CenterEventType::ConnectionLost);
        }
    });

    if (++m_auditTick >= 10) { m_auditTick = 0; SelfAuditRegistry(); }
    ETCD_LOG_INFO("EtcdMetrics — keepalive ok=" << m_metricKeepAliveOk
                  << " fail=" << m_metricKeepAliveFail
                  << " watchReconnect=" << m_metricWatchReconnect
                  << " watchCompactCancel=" << m_metricWatchCompactCancel
                  << " registryRebind=" << m_metricRegistryRebind
                  << " selfAuditFail=" << m_metricSelfAuditFail
                  << " lease=" << m_leaseId << " nodeId=" << m_nodeId);
}

// ============================================================
// 工具函数
// ============================================================

/*static*/
std::string EtcdCenterConnector::B64(const std::string& s)
{
    if (s.empty()) return {};

    // 防止 size_t → int 截断溢出（Base64encode_len 参数为 int）
    // 注: static 工具函数无实例 logger; 该边界(>1GB)实际不会触发, 返回空由调用方处理。
    if (s.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 2))
    {
        return {};
    }

    int outLen = Base64encode_len(static_cast<int>(s.size()));
    std::vector<char> buf(outLen);
    // Base64encode 会在末尾追加 '\0'，返回长度含 '\0'
    int written = Base64encode(buf.data(), s.data(), static_cast<int>(s.size()));
    // written 含末尾 '\0'，去掉
    if (written > 0 && buf[written - 1] == '\0') --written;
    // 去掉末尾换行（Apache base64 会插入换行）
    while (written > 0 && (buf[written - 1] == '\n' || buf[written - 1] == '\r'))
        --written;
    return std::string(buf.data(), written);
}

/*static*/
std::string EtcdCenterConnector::B64Dec(const std::string& s)
{
    if (s.empty()) return {};

    int outLen = Base64decode_len(s.c_str());
    std::vector<char> buf(outLen + 1, '\0');
    int written = Base64decode(buf.data(), s.c_str());
    return std::string(buf.data(), written);
}

void EtcdCenterConnector::Emit(CenterEventType type, CenterEvent ev)
{
    ev.type = type;
    if (m_callback) m_callback(ev);
}

// ============================================================
// ============================================================
// Watch 功能（Phase C: EtcdWatcher 替代 curl 线程 + ev_async, issus #24）
// ============================================================

void EtcdCenterConnector::StartWatch()
{
    if (!m_http) return;
    // 创建 EtcdWatcher: 读 host:port 从 m_endpoint
    auto protoEnd = m_endpoint.find("://");
    std::string hp = (protoEnd != std::string::npos) ? m_endpoint.substr(protoEnd + 3) : m_endpoint;
    auto colon = hp.rfind(':');
    if (colon == std::string::npos) { ETCD_LOG_ERROR("StartWatch — 端点格式错误"); return; }
    std::string host = hp.substr(0, colon);
    int port = 0; try { port = std::stoi(hp.substr(colon + 1)); } catch (...) {}
    if (port <= 0) { ETCD_LOG_ERROR("StartWatch — 端口解析失败"); return; }

    m_watcher = std::make_unique<EtcdWatcher>(m_loop, host, port, "/thunder/", m_logger,
        [this](const std::string& line) { ProcessWatchLine(line); });
    DoWatchSnapshot();  // 先取 snapshot 拿到 revision, 再 Start watcher
    ETCD_LOG_INFO("Watch — watcher 已启动(主循环,无线程)");
}

void EtcdCenterConnector::StopWatch()
{
    if (m_watcher) { m_watcher->Stop(); m_watcher.reset(); }
    ETCD_LOG_INFO("Watch — watcher 已停止");
}

void EtcdCenterConnector::DoWatchSnapshot()
{
    if (!m_http) return;
    std::string pfx  = "/thunder/";
    std::string rend = pfx; if (!rend.empty()) rend.back() = static_cast<char>(static_cast<unsigned char>(rend.back()) + 1);
    util::CJsonObject oReq;
    oReq.Add("key", B64(pfx));
    oReq.Add("range_end", B64(rend));
    m_http->Post("/v3/kv/range", oReq.ToString(),
        [this, pfx, rend](bool ok, int, const std::string& resp) {
            if (!ok || resp.empty()) { return; }
            util::CJsonObject oSnap; if (!oSnap.Parse(resp)) return;
            util::CJsonObject oHeader; int64_t snapRev = 0;
            if (oSnap.Get("header", oHeader) && etcd_parse::GetGatewayInt64(oHeader, "revision", snapRev) && snapRev > 0) {
                if (snapRev > m_lastRevision) m_lastRevision = snapRev;
            }
            // 加载全量 kvs 作为 PUT 事件
            // 全量快照前清空注册表, 避免残留旧 Pod IP (#issue: k8s pod 重启换 IP 后旧 key 无 DELETE 事件)
            util::CJsonObject kvs;
            if (oSnap.Get("kvs", kvs) && kvs.IsArray()) {
                m_nodeRegistry.clear();
                int n = kvs.GetArraySize();
                for (int i = 0; i < n; ++i) {
                    util::CJsonObject kv; if (!kvs.Get(i, kv)) continue;
                    std::string kB64, vB64; kv.Get("key", kB64); kv.Get("value", vB64);
                    m_watchQueue.push_back(WatchEvent{"PUT", B64Dec(kB64), B64Dec(vB64)});
                }
            }
            if (!m_watchQueue.empty()) OnWatchAsync();
            if (m_watcher) m_watcher->Start(m_lastRevision);
        });
}

void EtcdCenterConnector::ProcessWatchLine(const std::string& line)
{
    if (line.empty() || line[0] != '{') return;
    util::CJsonObject o;
    if (!o.Parse(line)) return;
    util::CJsonObject result;
    if (!o.Get("result", result)) return;

    bool created = false, canceled = false;
    result.Get("created", created); result.Get("canceled", canceled);
    if (canceled) {
        ++m_metricWatchCompactCancel;
        int64_t cr = 0;
        if (etcd_parse::GetGatewayInt64(result, "compact_revision", cr) && cr > m_lastRevision)
            m_lastRevision = cr;
        // compaction 取消 → 重做 snapshot + restart watcher
        DoWatchSnapshot();
        return;
    }
    if (created) return;

    util::CJsonObject events;
    if (!result.Get("events", events) || !events.IsArray()) return;
    int n = events.GetArraySize();
    for (int i = 0; i < n; ++i) {
        util::CJsonObject ev; if (!events.Get(i, ev)) continue;
        std::string evType; ev.Get("type", evType);
        util::CJsonObject kv; if (!ev.Get("kv", kv)) continue;
        std::string keyB64, valueB64; kv.Get("key", keyB64); kv.Get("value", valueB64);
        m_watchQueue.push_back(WatchEvent{evType, B64Dec(keyB64), B64Dec(valueB64)});
    }
    if (!m_watchQueue.empty()) OnWatchAsync();
}

void EtcdCenterConnector::OnWatchAsync()
{
    std::vector<WatchEvent> events; events.swap(m_watchQueue);  // 单线,无需锁
    static const std::string kConfigPrefix("/thunder/config/");

    for (const auto& wev : events) {
        if (wev.key.find(kConfigPrefix) == 0) {
            std::string configPath = wev.key.substr(kConfigPrefix.size());
            CenterEvent cev; cev.type = CenterEventType::ConfigUpdated;
            // etcd grpc-gateway 省略 PUT 的 type 字段: 空 type = PUT (#34)
            if (wev.type.empty() || wev.type == "PUT") { cev.config_content = wev.value; }
            else { ETCD_LOG_DEBUG("Watch — CONFIG DELETE " << configPath << " (忽略)"); continue; }
            m_callback(cev); continue;
        }
        if (wev.key.find(kRegistryPrefix) != 0) continue;
        // key 格式: /thunder/registry/[{TYPE}/]{IP}:{PORT} (#38 etcd key 重构)
        const std::string rest = wev.key.substr(strlen(kRegistryPrefix));
        auto slash = rest.find('/');
        std::string ntype, ipPort;
        if (slash != std::string::npos) {
            // 新格式: {TYPE}/{IP}:{PORT}
            ntype = rest.substr(0, slash);
            ipPort = rest.substr(slash + 1);
        } else {
            // 旧格式兼容: {IP}:{PORT} (type 从 value 中提取)
            ipPort = rest;
        }
        if (ipPort.rfind(':') == std::string::npos) continue;
        // 上游类型过滤在 key 层完成(新格式), 旧格式需事后过滤
        if (!m_upstreamTypes.empty() && !ntype.empty()
            && m_upstreamTypes.find(ntype) == m_upstreamTypes.end())
            continue;
        std::string evType = wev.type;
        if (evType.empty()) evType = "PUT";  // grpc-gateway 省略 type → 默认 PUT
        if (evType == "PUT") { m_nodeRegistry[ipPort] = wev.value; }
        else { m_nodeRegistry.erase(ipPort); }
    }
    if (events.empty()) return;

    // 构造路由快照(新格式已在 entry 处过滤, 此处兜底旧格式兼容)
    NodeNotice notice;
    for (const auto& kv : m_nodeRegistry) {
        const std::string& kvIpPort = kv.first;
        auto c = kvIpPort.rfind(':');
        if (c == std::string::npos) continue;
        util::CJsonObject oVal;
        if (!oVal.Parse(kv.second)) continue;
        int32_t nid = 0; uint32_t wnum = 0;
        oVal.Get("node_id", nid); oVal.Get("worker_num", wnum);
        std::string ntype;
        oVal.Get("node_type", ntype);
        // 旧格式兼容: 未在 entry 处过滤的类型在此处过滤
        if (!m_upstreamTypes.empty() && !ntype.empty()
            && m_upstreamTypes.find(ntype) == m_upstreamTypes.end())
            continue;
        auto* nr = notice.add_node_arry_reg();
        nr->set_node_ip(kvIpPort.substr(0, c));
        nr->set_node_port(static_cast<uint32_t>(std::stoul(kvIpPort.substr(c + 1))));
        if (nid > 0) nr->set_node_id(static_cast<uint32_t>(nid));
        if (!ntype.empty()) nr->set_node_type(ntype);
        nr->set_worker_num(wnum > 0 ? wnum : 1);
    }
    CenterEvent cev; cev.type = CenterEventType::RouteUpdated;
    cev.route_snapshot = notice.SerializeAsString();
    m_callback(cev);
}

} /* namespace net */
