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

#include <cstring>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <ev.h>

#include "protocol/oss_sys.pb.h"
#include "util/json/CJsonObject.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// ---- 日志宏（独立 category，避免依赖 GetLabor() 上下文） ----
namespace
{
log4cplus::Logger& GetEtcdLogger()
{
    static log4cplus::Logger logger =
        log4cplus::Logger::getInstance("EtcdCenterConnector");
    return logger;
}
}  // namespace

#define ETCD_LOG_DEBUG(msg) LOG4CPLUS_DEBUG(GetEtcdLogger(), msg)
#define ETCD_LOG_INFO(msg)  LOG4CPLUS_INFO(GetEtcdLogger(),  msg)
#define ETCD_LOG_WARN(msg)  LOG4CPLUS_WARN(GetEtcdLogger(),  msg)
#define ETCD_LOG_ERROR(msg) LOG4CPLUS_ERROR(GetEtcdLogger(), msg)

// ---- etcd key 命名空间 ----
static constexpr const char* kSlotPrefix     = "/thunder/slot/";
static constexpr const char* kRegistryPrefix = "/thunder/registry/";
static constexpr int         kMaxSlot        = 255;
static constexpr int         kLeaseTTL       = 10;   ///< 秒
static constexpr double      kKeepAliveInterval = 3.0; ///< 秒

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
    if (m_keepAliveTimer)
    {
        if (m_loop)
        {
            ev_timer_stop(m_loop, m_keepAliveTimer);
        }
        delete m_keepAliveTimer;
        m_keepAliveTimer = nullptr;
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

    // 申请租约
    if (!LeaseGrant())
    {
        ETCD_LOG_ERROR("EtcdCenterConnector::Init — LeaseGrant 失败");
        return false;
    }

    // 启动 KeepAlive 定时器（每 kKeepAliveInterval 秒续租）
    m_keepAliveTimer = new ev_timer();
    m_keepAliveTimer->data = static_cast<void*>(this);
    ev_timer_init(m_keepAliveTimer, KeepAliveTimerCallback,
                  kKeepAliveInterval, kKeepAliveInterval);
    ev_timer_start(m_loop, m_keepAliveTimer);

    ETCD_LOG_INFO("EtcdCenterConnector::Init — 成功，leaseId=" << m_leaseId);
    return true;
}

// ============================================================
// Destroy
// ============================================================

void EtcdCenterConnector::Destroy()
{
    // 停止定时器
    if (m_keepAliveTimer)
    {
        if (m_loop)
        {
            ev_timer_stop(m_loop, m_keepAliveTimer);
        }
        delete m_keepAliveTimer;
        m_keepAliveTimer = nullptr;
    }

    // 撤销租约（会级联删除所有关联的 key）
    if (m_leaseId != 0)
    {
        LeaseRevoke();
        m_leaseId    = 0;
        m_registered = false;
    }

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

    if (is_register || !m_registered)
    {
        // 注册流程：槽位占位 or 幂等续期
        DoRegister(ip, port, type);
    }
    else
    {
        // 心跳：仅续租
        if (!KeepAlive())
        {
            ETCD_LOG_WARN("EtcdCenterConnector::ReportNodeStatus — 心跳续租失败");
            Emit(CenterEventType::ConnectionLost);
            return false;
        }
        ETCD_LOG_DEBUG("EtcdCenterConnector::ReportNodeStatus — 心跳成功 nodeId=" << m_nodeId);
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

std::string EtcdCenterConnector::EtcdPost(const std::string& path,
                                           const std::string& body)
{
    const std::string url = m_endpoint + path;
    std::string response;
    // eContentType_json = 1
    CURLcode code = m_curl.PostHttps(url, body, response,
                                     "",  // no auth
                                     util::CurlClient::eContentType_json);
    if (code != CURLE_OK)
    {
        ETCD_LOG_WARN("EtcdPost failed url=" << url << " curl_code=" << code);
        return {};
    }
    return response;
}

bool EtcdCenterConnector::LeaseGrant()
{
    // 构建请求体 {"TTL": 10}
    util::CJsonObject oReq;
    oReq.Add("TTL", static_cast<int32_t>(kLeaseTTL));

    std::string resp = EtcdPost("/v3/lease/grant", oReq.ToString());
    if (resp.empty())
    {
        ETCD_LOG_ERROR("LeaseGrant — HTTP 请求失败");
        return false;
    }

    util::CJsonObject oResp;
    if (!oResp.Parse(resp))
    {
        ETCD_LOG_ERROR("LeaseGrant — 响应 JSON 解析失败: " << resp);
        return false;
    }

    // etcd gateway 返回 ID 为字符串形式的 int64
    std::string leaseIdStr;
    if (!oResp.Get("ID", leaseIdStr) || leaseIdStr.empty())
    {
        ETCD_LOG_ERROR("LeaseGrant — 响应中无 ID 字段: " << resp);
        return false;
    }

    try
    {
        m_leaseId = std::stoll(leaseIdStr);
    }
    catch (const std::exception& e)
    {
        ETCD_LOG_ERROR("LeaseGrant — ID 转换失败: " << leaseIdStr << " err=" << e.what());
        return false;
    }

    ETCD_LOG_INFO("LeaseGrant — leaseId=" << m_leaseId);
    return true;
}

bool EtcdCenterConnector::KeepAlive()
{
    if (m_leaseId == 0) return false;

    // {"ID": "<leaseId>"}
    util::CJsonObject oReq;
    oReq.Add("ID", std::to_string(m_leaseId));

    std::string resp = EtcdPost("/v3/lease/keepalive", oReq.ToString());
    if (resp.empty())
    {
        ETCD_LOG_WARN("KeepAlive — HTTP 请求失败 leaseId=" << m_leaseId);
        return false;
    }

    // 简单检查响应中是否包含 result.ID（说明续租成功）
    // 响应格式: {"result":{"ID":"...","TTL":"10"}}
    util::CJsonObject oResp;
    if (!oResp.Parse(resp))
    {
        ETCD_LOG_WARN("KeepAlive — 响应解析失败: " << resp);
        return false;
    }

    util::CJsonObject oResult;
    if (!oResp.Get("result", oResult))
    {
        ETCD_LOG_WARN("KeepAlive — 响应无 result 字段: " << resp);
        return false;
    }

    ETCD_LOG_DEBUG("KeepAlive — 成功 leaseId=" << m_leaseId);
    return true;
}

void EtcdCenterConnector::LeaseRevoke()
{
    if (m_leaseId == 0) return;

    util::CJsonObject oReq;
    oReq.Add("ID", std::to_string(m_leaseId));

    std::string resp = EtcdPost("/v3/lease/revoke", oReq.ToString());
    if (resp.empty())
    {
        ETCD_LOG_WARN("LeaseRevoke — 失败（忽略），leaseId=" << m_leaseId);
    }
    else
    {
        ETCD_LOG_INFO("LeaseRevoke — 成功 leaseId=" << m_leaseId);
    }
}

// ============================================================
// 注册核心逻辑
// ============================================================

void EtcdCenterConnector::DoRegister(const std::string& ip,
                                      uint32_t port,
                                      const std::string& nodeType)
{
    (void)nodeType;  // 当前注册不存储 nodeType 到 etcd，保留扩展

    const std::string registryKey =
        std::string(kRegistryPrefix) + ip + ":" + std::to_string(port);

    // ---- Step 1: 幂等查询 —— 已有 registry/ip:port 则续期返回 ----
    {
        util::CJsonObject oReq;
        oReq.Add("key", B64(registryKey));

        std::string resp = EtcdPost("/v3/kv/range", oReq.ToString());
        if (!resp.empty())
        {
            util::CJsonObject oResp;
            if (oResp.Parse(resp))
            {
                // count 字段是字符串形式，如 "1"
                std::string countStr;
                int count = 0;
                if (oResp.Get("count", countStr) && !countStr.empty())
                {
                    try { count = std::stoi(countStr); }
                    catch (...) { count = 0; }
                }

                if (count > 0)
                {
                    // 已注册：取出 node_id
                    util::CJsonObject oKvs;
                    if (oResp.Get("kvs", oKvs) && oKvs.IsArray())
                    {
                        util::CJsonObject oKv;
                        if (oKvs.Get(0, oKv))
                        {
                            std::string valueB64;
                            if (oKv.Get("value", valueB64))
                            {
                                std::string valueStr = B64Dec(valueB64);
                                try
                                {
                                    m_nodeId     = static_cast<uint32_t>(std::stoul(valueStr));
                                    m_registered = true;

                                    // 续租一次，确保租约新鲜
                                    KeepAlive();

                                    ETCD_LOG_INFO("DoRegister — 已注册(幂等)，续期 nodeId="
                                                  << m_nodeId);

                                    CenterEvent ev;
                                    ev.node_id = m_nodeId;
                                    Emit(CenterEventType::Registered, ev);
                                    return;
                                }
                                catch (...)
                                {
                                    ETCD_LOG_WARN("DoRegister — value 解析失败: " << valueStr);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Step 2: 抢槽位 —— 从 hash(ip:port)%255+1 开始顺序扫描 ----
    // 简单哈希：对 ip+port 字符串按字节累加
    std::string hashSrc = ip + std::to_string(port);
    uint32_t hashVal = 0;
    for (unsigned char c : hashSrc) hashVal += c;
    const int startSlot = static_cast<int>(hashVal % kMaxSlot) + 1;  // [1..255]

    for (int loop = 0; loop < kMaxSlot; ++loop)
    {
        // 从 startSlot 开始循环扫，slot 范围 [1..255]
        int slotIdx = ((startSlot - 1 + loop) % kMaxSlot) + 1;
        const std::string slotKey =
            std::string(kSlotPrefix) + std::to_string(slotIdx);

        // 构建 txn：compare slot/i 的 create_revision == 0（即 key 不存在）
        //   success: put slot/i = ip:port，put registry/ip:port = slotIdx
        //   failure: range slot/i（查看当前值，不实际使用）
        util::CJsonObject oTxn;

        // compare 数组
        util::CJsonObject oCompare;
        util::CJsonObject oCmp;
        oCmp.Add("key",             B64(slotKey));
        oCmp.Add("target",          std::string("CREATE"));
        oCmp.Add("result",          std::string("EQUAL"));
        oCmp.Add("create_revision", std::string("0"));
        oCompare.Add(oCmp);
        oTxn.Add("compare", oCompare);

        // success 数组：两个 put
        util::CJsonObject oSuccess;

        // put slot/i = ip:port
        util::CJsonObject oPutSlot;
        util::CJsonObject oPutSlotKV;
        oPutSlotKV.Add("key",   B64(slotKey));
        oPutSlotKV.Add("value", B64(ip + ":" + std::to_string(port)));
        oPutSlotKV.Add("lease", std::to_string(m_leaseId));
        oPutSlot.Add("request_put", oPutSlotKV);
        oSuccess.Add(oPutSlot);

        // put registry/ip:port = slotIdx（node_id）
        util::CJsonObject oPutReg;
        util::CJsonObject oPutRegKV;
        oPutRegKV.Add("key",   B64(registryKey));
        oPutRegKV.Add("value", B64(std::to_string(slotIdx)));
        oPutRegKV.Add("lease", std::to_string(m_leaseId));
        oPutReg.Add("request_put", oPutRegKV);
        oSuccess.Add(oPutReg);

        oTxn.Add("success", oSuccess);

        // failure 数组：range slot/i（不关心结果，仅满足协议格式）
        util::CJsonObject oFailure;
        util::CJsonObject oRangeFail;
        util::CJsonObject oRangeFailKV;
        oRangeFailKV.Add("key", B64(slotKey));
        oRangeFail.Add("request_range", oRangeFailKV);
        oFailure.Add(oRangeFail);
        oTxn.Add("failure", oFailure);

        std::string resp = EtcdPost("/v3/kv/txn", oTxn.ToString());
        if (resp.empty())
        {
            ETCD_LOG_WARN("DoRegister — txn HTTP 失败 slot=" << slotIdx);
            continue;
        }

        util::CJsonObject oResp;
        if (!oResp.Parse(resp))
        {
            ETCD_LOG_WARN("DoRegister — txn 响应解析失败: " << resp);
            continue;
        }

        bool succeeded = false;
        oResp.Get("succeeded", succeeded);  // bool 类型，CJsonObject 可直接取
        if (succeeded)
        {
            m_nodeId     = static_cast<uint32_t>(slotIdx);
            m_registered = true;

            ETCD_LOG_INFO("DoRegister — 注册成功 nodeId=" << m_nodeId
                          << " slot=" << slotKey
                          << " registryKey=" << registryKey);

            CenterEvent ev;
            ev.node_id = m_nodeId;
            Emit(CenterEventType::Registered, ev);
            return;
        }
        // txn 失败（slot 已被占用），继续下一个 slot
        ETCD_LOG_DEBUG("DoRegister — slot=" << slotIdx << " 已占用，尝试下一个");
    }

    // 所有槽位已满
    ETCD_LOG_ERROR("DoRegister — 所有槽位已满（max=" << kMaxSlot << "）");
    CenterEvent ev;
    ev.errcode = 1;
    ev.errmsg  = "no slot available";
    Emit(CenterEventType::Registered, ev);
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
    if (m_leaseId == 0 || !m_registered) return;

    if (!KeepAlive())
    {
        ETCD_LOG_WARN("OnKeepAliveTimer — 续租失败 leaseId=" << m_leaseId
                      << "，触发 ConnectionLost");
        m_registered = false;
        Emit(CenterEventType::ConnectionLost);
    }
}

// ============================================================
// 工具函数
// ============================================================

/*static*/
std::string EtcdCenterConnector::B64(const std::string& s)
{
    if (s.empty()) return {};

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

} /* namespace net */
