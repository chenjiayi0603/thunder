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
#include "EtcdParse.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <curl/curl.h>
#include "protocol/oss_sys.pb.h"
#include "util/json/CJsonObject.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// ---- curl RAII 包装（避免 EtcdPost 中的提前返回导致资源泄漏） ----
namespace
{

struct CurlHandle
{
    CURL* h;
    CurlHandle() : h(curl_easy_init()) {}
    ~CurlHandle() { if (h) curl_easy_cleanup(h); }
    CurlHandle(const CurlHandle&)            = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    operator CURL*() const { return h; }  // NOLINT(google-explicit-constructor)
};

struct SlistHandle
{
    curl_slist* s = nullptr;
    SlistHandle()  = default;
    ~SlistHandle() { curl_slist_free_all(s); }
    SlistHandle(const SlistHandle&)            = delete;
    SlistHandle& operator=(const SlistHandle&) = delete;
};

}  // namespace

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

    // 申请租约
    if (!LeaseGrant())
    {
        ETCD_LOG_ERROR("EtcdCenterConnector::Init — LeaseGrant 失败");
        return false;
    }

    // 启动 KeepAlive 定时器（每 kKeepAliveInterval 秒续租，值成员无需 new）
    m_keepAliveTimer.data = static_cast<void*>(this);
    ev_timer_init(&m_keepAliveTimer, KeepAliveTimerCallback,
                  kKeepAliveInterval, kKeepAliveInterval);
    ev_timer_start(m_loop, &m_keepAliveTimer);
    m_keepAliveTimerStarted = true;

    // Phase 2: 注册 ev_async，用于 watch 线程 → libev 线程的跨线程唤醒
    m_watchAsync.data = static_cast<void*>(this);
    ev_async_init(&m_watchAsync, WatchAsyncCallback);
    ev_async_start(m_loop, &m_watchAsync);
    m_watchAsyncStarted = true;

    // Phase 2: 启动 watch 线程（在 libev 线程之外运行 libcurl 长连接）
    StartWatch();

    ETCD_LOG_INFO("EtcdCenterConnector::Init — 成功，leaseId=" << m_leaseId);
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

    // 停止 watch 线程
    StopWatch();

    // 注销 watch 用的 ev_async
    if (m_watchAsyncStarted && m_loop)
    {
        ev_async_stop(m_loop, &m_watchAsync);
        m_watchAsyncStarted = false;
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
    m_workerNum = report.worker_num();  // 写入 registry value, 供对端建路由 identify

    if (is_register || !m_registered)
    {
        // 注册流程（首次或显式注册）：槽位占位 or 幂等续期
        DoRegister(ip, port, type);
    }
    else
    {
        // 心跳：仅在已注册且 lease 有效时续租
        if (m_registered && m_leaseId != 0)
        {
            if (!KeepAlive())
            {
                ETCD_LOG_WARN("EtcdCenterConnector::ReportNodeStatus — 心跳续租失败");
                Emit(CenterEventType::ConnectionLost);
                return false;
            }
            ETCD_LOG_DEBUG("EtcdCenterConnector::ReportNodeStatus — 心跳成功 nodeId=" << m_nodeId);
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

/*static*/
size_t EtcdCenterConnector::CurlWriteCallback(void* ptr, size_t size,
                                               size_t nmemb, void* userdata)
{
    auto* str = static_cast<std::string*>(userdata);
    str->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::string EtcdCenterConnector::EtcdPost(const std::string& path,
                                           const std::string& body)
{
    CurlHandle curl;
    if (!curl.h)
    {
        ETCD_LOG_WARN("EtcdPost — curl_easy_init() 失败");
        return {};
    }

    const std::string url = m_endpoint + path;
    std::string response;

    SlistHandle hdrs;
    hdrs.s = curl_slist_append(hdrs.s, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,               url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,              1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,        body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,     static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,        hdrs.s);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,     CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,         &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,        2000L);  // etcd 故障时最多阻塞 2s
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    // 注意：不设置 CURLOPT_SSL_VERIFYPEER/VERIFYHOST。
    //   当前 etcd 端点为 http://，无需 TLS；
    //   若未来切换到 https://，应正确配置 CA 而非禁用验证（防 MITM）。

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK)
    {
        ETCD_LOG_WARN("EtcdPost failed url=" << url
                      << " err=" << curl_easy_strerror(res));
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
        ++m_metricKeepAliveFail;
        return false;
    }

    // 响应格式: {"result":{"ID":"...","TTL":"10"}}
    util::CJsonObject oResp;
    if (!oResp.Parse(resp))
    {
        ETCD_LOG_WARN("KeepAlive — 响应解析失败: " << resp);
        ++m_metricKeepAliveFail;
        return false;
    }

    util::CJsonObject oResult;
    if (!oResp.Get("result", oResult))
    {
        ETCD_LOG_WARN("KeepAlive — 响应无 result 字段: " << resp);
        ++m_metricKeepAliveFail;
        return false;
    }

    // 必须校验 TTL > 0：etcd 对【已过期/不存在】的 lease 也回 200，但 TTL=0。
    // 旧代码只看 result 存在 → 把死租约误判为续租成功，掩盖注册丢失 (issus #19)。
    int64_t ttl = 0;
    if (!etcd_parse::GetGatewayInt64(oResult, "TTL", ttl) || ttl <= 0)
    {
        ETCD_LOG_WARN("KeepAlive — 租约已失效(TTL=" << ttl << ") leaseId=" << m_leaseId);
        ++m_metricKeepAliveFail;
        return false;
    }

    ++m_metricKeepAliveOk;
    ETCD_LOG_DEBUG("KeepAlive — 成功 leaseId=" << m_leaseId << " ttl=" << ttl);
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

bool EtcdCenterConnector::QueryRegistry(const std::string& registryKey,
                                         uint32_t& outNodeId, int64_t& outLease)
{
    outNodeId = 0;
    outLease  = 0;

    util::CJsonObject oReq;
    oReq.Add("key", B64(registryKey));

    const std::string resp = EtcdPost("/v3/kv/range", oReq.ToString());
    if (resp.empty()) return false;

    util::CJsonObject oResp;
    if (!oResp.Parse(resp)) return false;

    // count 字段是字符串形式，如 "1"
    std::string countStr;
    int count = 0;
    if (oResp.Get("count", countStr) && !countStr.empty())
    {
        try { count = std::stoi(countStr); }
        catch (...) { count = 0; }
    }
    if (count <= 0) return false;

    util::CJsonObject oKvs;
    if (!oResp.Get("kvs", oKvs) || !oKvs.IsArray()) return false;

    util::CJsonObject oKv;
    if (!oKvs.Get(0, oKv)) return false;

    // 取出该 key 绑定的 lease（gateway int64 字符串，无租约为 "0"/缺省）。
    // issus #19 关键：据此判断键是否还绑在【当前】租约上。
    etcd_parse::GetGatewayInt64(oKv, "lease", outLease);

    std::string valueB64;
    if (!oKv.Get("value", valueB64)) return false;

    const std::string valueStr = B64Dec(valueB64);
    // registry value 是 JSON 格式：{"node_id":7, "node_type":"HELLO", ...}
    util::CJsonObject oVal;
    if (oVal.Parse(valueStr))
    {
        int32_t nid = 0;
        if (oVal.Get("node_id", nid) && nid > 0 && nid <= static_cast<int32_t>(kMaxSlot))
        {
            outNodeId = static_cast<uint32_t>(nid);
            return true;
        }
    }
    // 兼容旧格式：value 是纯数字字符串
    try
    {
        outNodeId = static_cast<uint32_t>(std::stoul(valueStr));
        return true;
    }
    catch (...)
    {
        ETCD_LOG_WARN("QueryRegistry — value 解析失败: " << valueStr);
        return false;
    }
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

bool EtcdCenterConnector::TryClaimSlot(int slot,
                                        const std::string& slotKey,
                                        const std::string& registryKey,
                                        const std::string& ipPort,
                                        const std::string& nodeType)
{
    const std::string txnBody = BuildSlotTxn(slot, slotKey, registryKey, ipPort, nodeType);
    const std::string resp    = EtcdPost("/v3/kv/txn", txnBody);
    if (resp.empty())
    {
        ETCD_LOG_WARN("TryClaimSlot — txn HTTP 失败 slot=" << slot);
        return false;
    }

    util::CJsonObject oResp;
    if (!oResp.Parse(resp))
    {
        ETCD_LOG_WARN("TryClaimSlot — txn 响应解析失败: " << resp);
        return false;
    }

    bool succeeded = false;
    oResp.Get("succeeded", succeeded);
    return succeeded;
}

void EtcdCenterConnector::DoRegister(const std::string& ip,
                                      uint32_t port,
                                      const std::string& nodeType)
{
    // 必须持有有效租约才能写注册键，否则写出 lease=0 的孤儿键：永不过期、跨重启累积
    // 占满槽位 → "所有槽位已满" → 路由失效。etcd 重启瞬间 Init 的 LeaseGrant 可能失败
    // 导致 m_leaseId=0，这里兜底重试一次；仍失败则放弃本轮，交由 KeepAlive 定时器重试。
    if (m_leaseId == 0 && !LeaseGrant())
    {
        ETCD_LOG_ERROR("DoRegister — 无有效 lease 且补领失败，放弃本次注册（待定时器重试）");
        return;
    }

    const std::string ipPort      = ip + ":" + std::to_string(port);
    const std::string registryKey = std::string(kRegistryPrefix) + ipPort;

    // ---- Step 1: 幂等查询 + 租约归属判定（issus #19） ----
    // 老 bug：只要 registry/ip:port 存在就当"已注册"返回，不管它绑在哪个租约。
    // 重启换新租约后，etcd 里残留的是【旧租约】绑的键；旧租约一过期 etcd 就删键，
    // 而本进程 m_registered=true 永不重注册 → 注册中心永久为空。
    // 正解：键绑在当前租约才算 Fresh；绑在别的(旧/失效)租约必须 Rebind 到当前租约。
    uint32_t existingNodeId = 0;
    int64_t  existingLease  = 0;
    const bool found = QueryRegistry(registryKey, existingNodeId, existingLease);
    const auto action = etcd_parse::DecideRegAction(found, existingLease, m_leaseId);

    if (action == etcd_parse::RegAction::Fresh)
    {
        m_nodeId     = existingNodeId;
        m_registered = true;
        KeepAlive();  // 续租一次，确保租约新鲜
        ETCD_LOG_INFO("DoRegister — 已注册(幂等,租约一致)，续期 nodeId=" << m_nodeId
                      << " lease=" << m_leaseId);
        CenterEvent ev;
        ev.node_id = m_nodeId;
        Emit(CenterEventType::Registered, std::move(ev));
        return;
    }
    if (action == etcd_parse::RegAction::Rebind)
    {
        ETCD_LOG_WARN("DoRegister — 注册键残留在旧租约(existingLease=" << existingLease
                      << " != 当前 lease=" << m_leaseId << ")，重绑到当前租约 nodeId="
                      << existingNodeId);
        m_nodeId = existingNodeId;
        if (RebindRegistration(existingNodeId, registryKey, ipPort, nodeType))
        {
            m_registered = true;
            ++m_metricRegistryRebind;
            CenterEvent ev;
            ev.node_id = m_nodeId;
            Emit(CenterEventType::Registered, std::move(ev));
            return;
        }
        ETCD_LOG_ERROR("DoRegister — 重绑失败，回退到槽位抢占");
        // 重绑失败则继续走 Step 2 抢占（兜底）
    }

    // ---- Step 2: 抢槽位 —— 从 hash(ip:port)%255+1 开始顺序扫描 ----
    // 简单哈希：对 ip+port 字符串按字节累加
    uint32_t hashVal = 0;
    for (unsigned char c : ipPort) hashVal += c;
    const int startSlot = static_cast<int>(hashVal % kMaxSlot) + 1;  // [1..255]

    for (uint32_t loop = 0; loop < kMaxSlot; ++loop)
    {
        const int slotIdx = static_cast<int>((static_cast<uint32_t>(startSlot - 1) + loop) % kMaxSlot) + 1;
        const std::string slotKey = std::string(kSlotPrefix) + std::to_string(slotIdx);

        if (TryClaimSlot(slotIdx, slotKey, registryKey, ipPort, nodeType))
        {
            m_nodeId     = static_cast<uint32_t>(slotIdx);
            m_registered = true;
            ETCD_LOG_INFO("DoRegister — 注册成功 nodeId=" << m_nodeId
                          << " slot=" << slotKey
                          << " registryKey=" << registryKey);
            CenterEvent ev;
            ev.node_id = m_nodeId;
            Emit(CenterEventType::Registered, std::move(ev));
            return;
        }
        ETCD_LOG_DEBUG("DoRegister — slot=" << slotIdx << " 已占用，尝试下一个");
    }

    // 所有槽位已满
    ETCD_LOG_ERROR("DoRegister — 所有槽位已满（max=" << kMaxSlot << "）");
    CenterEvent ev;
    ev.errcode = 1;
    ev.errmsg  = "no slot available";
    Emit(CenterEventType::Registered, std::move(ev));
}

bool EtcdCenterConnector::RebindRegistration(uint32_t nodeId,
                                             const std::string& registryKey,
                                             const std::string& ipPort,
                                             const std::string& nodeType)
{
    if (m_leaseId == 0)
    {
        ETCD_LOG_ERROR("RebindRegistration — 当前无 lease，无法重绑");
        return false;
    }
    const std::string slotKey = std::string(kSlotPrefix) + std::to_string(nodeId);

    // 无条件 PUT，把 key 重新绑定到【当前】m_leaseId（PUT 带 lease 会覆盖旧的 lease 绑定）。
    auto putKv = [&](const std::string& key, const std::string& val) -> bool {
        util::CJsonObject o;
        o.Add("key",   B64(key));
        o.Add("value", B64(val));
        o.Add("lease", std::to_string(m_leaseId));
        return !EtcdPost("/v3/kv/put", o.ToString()).empty();
    };

    if (!putKv(slotKey, ipPort))
    {
        ETCD_LOG_ERROR("RebindRegistration — slot PUT 失败 slotKey=" << slotKey);
        return false;
    }
    const std::string regValue = BuildRegistryValueJson(static_cast<int>(nodeId), ipPort,
                                                        nodeType, m_workerNum);
    if (!putKv(registryKey, regValue))
    {
        ETCD_LOG_ERROR("RebindRegistration — registry PUT 失败 key=" << registryKey);
        return false;
    }
    ETCD_LOG_INFO("RebindRegistration — 已重绑 slot+registry 到 lease=" << m_leaseId
                  << " nodeId=" << nodeId);
    return true;
}

void EtcdCenterConnector::SelfAuditRegistry()
{
    // 自检对账（issus #19 监控）：注册状态下，自身注册键必须存在且绑在当前租约。
    if (!m_registered || m_leaseId == 0 || m_nodeIp.empty()) return;

    const std::string ipPort      = m_nodeIp + ":" + std::to_string(m_nodePort);
    const std::string registryKey = std::string(kRegistryPrefix) + ipPort;

    uint32_t nid   = 0;
    int64_t  lease = 0;
    const bool found = QueryRegistry(registryKey, nid, lease);
    if (found && lease == m_leaseId)
    {
        return;  // 健康
    }

    ++m_metricSelfAuditFail;
    if (!found)
    {
        ETCD_LOG_ERROR("SelfAudit — 注册键已从 etcd 消失(注册中心将失效!) key=" << registryKey
                       << "，立即重绑 lease=" << m_leaseId);
    }
    else
    {
        ETCD_LOG_ERROR("SelfAudit — 注册键绑在错误租约(lease=" << lease
                       << " != 当前=" << m_leaseId << ") key=" << registryKey << "，立即重绑");
    }
    // 自愈：重绑到当前租约。nodeId 取已知 m_nodeId（找到则用 etcd 里的）。
    const uint32_t useNodeId = found && nid > 0 ? nid : m_nodeId;
    if (RebindRegistration(useNodeId, registryKey, ipPort, m_nodeType))
    {
        ++m_metricRegistryRebind;
    }
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
    if (m_leaseId == 0)
    {
        // 启动时 etcd 不可用 → lease 未建立。持续补领并重注册，直到成功（自愈）。
        // 否则节点永远不会注册到 etcd（旧代码此处直接 return，永不重试）。
        if (!m_nodeIp.empty() && LeaseGrant())
        {
            ETCD_LOG_INFO("OnKeepAliveTimer — 补领 lease 成功，重新注册 leaseId=" << m_leaseId);
            DoRegister(m_nodeIp, m_nodePort, m_nodeType);
        }
        return;
    }

    if (!m_registered)
    {
        // etcd 断连后尝试自动恢复：先续租，成功则恢复 registered 状态
        if (KeepAlive())
        {
            m_registered = true;
            m_keepAliveFailCount = 0;
            ETCD_LOG_INFO("OnKeepAliveTimer — etcd 恢复，续租成功 leaseId=" << m_leaseId);
            Emit(CenterEventType::ConnectionRestored);
            return;
        }
        ++m_keepAliveFailCount;
        // 续租连续失败超过 10 次（~30s），lease 可能已过期，重注册
        if (m_keepAliveFailCount >= 10)
        {
            ETCD_LOG_WARN("OnKeepAliveTimer — 续租连续失败 "
                          << m_keepAliveFailCount << " 次, 撤销旧 lease=" << m_leaseId
                          << " 尝试重注册");
            LeaseRevoke();
            m_leaseId = 0;
            m_keepAliveFailCount = 0;
            // 重注册：DoRegister 会重新 LeaseGrant + 幂等查 registry
            DoRegister(m_nodeIp, m_nodePort, m_nodeType);
        }
        return;
    }

    if (!KeepAlive())
    {
        ETCD_LOG_WARN("OnKeepAliveTimer — 续租失败 leaseId=" << m_leaseId
                      << "，触发 ConnectionLost");
        m_registered = false;
        m_keepAliveFailCount = 1;
        Emit(CenterEventType::ConnectionLost);
        return;
    }
    m_keepAliveFailCount = 0;

    // ---- 监控（issus #19/#20）：注册表自检对账 + 周期性指标汇总 ----
    // KeepAlive 定时器约每 3s 触发；自检每 ~30s 一次（10 拍）即可，避免额外 etcd 压力。
    if (++m_auditTick >= 10)
    {
        m_auditTick = 0;
        SelfAuditRegistry();
        ETCD_LOG_INFO("EtcdMetrics — keepalive ok=" << m_metricKeepAliveOk.load()
                      << " fail=" << m_metricKeepAliveFail.load()
                      << " watchReconnect=" << m_metricWatchReconnect.load()
                      << " watchCompactCancel=" << m_metricWatchCompactCancel.load()
                      << " registryRebind=" << m_metricRegistryRebind.load()
                      << " selfAuditFail=" << m_metricSelfAuditFail.load()
                      << " lease=" << m_leaseId << " nodeId=" << m_nodeId);
    }
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
// Watch 功能（Phase 2）
// ============================================================

void EtcdCenterConnector::StartWatch()
{
    m_lastRevision = 0;
    m_watchStop    = false;
    m_watchThread  = std::thread([this]{ WatchThreadFunc(); });
    ETCD_LOG_INFO("Watch — 线程已启动");
}

void EtcdCenterConnector::StopWatch()
{
    m_watchStop = true;
    if (m_watchThread.joinable()) m_watchThread.join();
    ETCD_LOG_INFO("Watch — 线程已停止");
}

void EtcdCenterConnector::WatchThreadFunc()
{
    // watch /thunder/ 共同前缀（覆盖 registry 和 config）
    static const std::string kWatcherPrefix = "/thunder/";
    const std::string prefix    = kWatcherPrefix;
    std::string       rangeEnd  = prefix;
    if (!rangeEnd.empty())
    {
        // etcd prefix watch: range_end = prefix 的最后一个字节 +1
        rangeEnd.back() = static_cast<char>(static_cast<unsigned char>(rangeEnd.back()) + 1);
    }

    int errBackoffSec = 1;  // etcd 真·故障时的递增退避（上限 8s），正常路径不使用

    while (!m_watchStop)
    {
        // ── 全量快照 + 取 watch 起点 revision ──
        // 关键修复:原先 m_lastRevision=0 → watch 从 start_revision=1 发起,
        // 一旦 etcd 发生 compaction(自动/长跑必然),rev 1 < compact_revision,
        // etcd 立即以 {"canceled":true,"compact_revision":N} 取消 watch,
        // 客户端永远收不到事件 → 跨节点路由全断 (issus #9)。
        // 正确做法:先 range 拉取现有 /thunder/ 全量,载入路由表(作为 PUT 事件),
        // 并以 range 返回的 header.revision 作为 watch 起点,绕开 compaction。
        // 每次(重)连都重做,保证断线/compaction 后自动重新同步。
        {
            util::CJsonObject oReq;
            oReq.Add("key", B64(prefix));
            oReq.Add("range_end", B64(rangeEnd));
            const std::string snapResp = EtcdPost("/v3/kv/range", oReq.ToString());
            util::CJsonObject oSnap;
            if (!snapResp.empty() && oSnap.Parse(snapResp))
            {
                util::CJsonObject oHeader;
                int64_t snapRev = 0;
                if (oSnap.Get("header", oHeader) &&
                    etcd_parse::GetGatewayInt64(oHeader, "revision", snapRev) && snapRev > 0)
                {
                    m_lastRevision = snapRev;
                }
                util::CJsonObject oKvs;
                int loaded = 0;
                if (oSnap.Get("kvs", oKvs) && oKvs.IsArray())
                {
                    const int n = oKvs.GetArraySize();
                    for (int i = 0; i < n; ++i)
                    {
                        util::CJsonObject oKv;
                        if (!oKvs.Get(i, oKv)) continue;
                        std::string kB64, vB64;
                        oKv.Get("key", kB64);
                        oKv.Get("value", vB64);
                        WatchEvent wev;
                        wev.type  = "PUT";
                        wev.key   = B64Dec(kB64);
                        wev.value = B64Dec(vB64);
                        {
                            std::lock_guard<std::mutex> lock(m_watchQueueMutex);
                            m_watchQueue.push_back(std::move(wev));
                        }
                        ++loaded;
                    }
                }
                if (loaded > 0) ev_async_send(m_loop, &m_watchAsync);
            }
        }

        CurlHandle curl;
        if (!curl.h)
        {
            ETCD_LOG_WARN("Watch — curl_easy_init 失败，3s 后重试");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        const std::string basePrefix   = B64(prefix);
        const std::string baseRangeEnd = B64(rangeEnd);

        std::ostringstream body;
        body << "{\"create_request\":{"
             << "\"key\":\""          << basePrefix   << "\","
             << "\"range_end\":\""    << baseRangeEnd << "\","
             << "\"start_revision\":" << (m_lastRevision + 1)
             << "}}";

        const std::string url = m_endpoint + "/v3/watch";

        SlistHandle hdrs;
        hdrs.s = curl_slist_append(hdrs.s, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL,               url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST,              1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,        body.str().c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,     static_cast<long>(body.str().size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,        hdrs.s);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,     WatchWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,         this);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,           0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,     1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,      30L);

        m_watchCompactRevision = 0;       // 本轮若被 compaction 取消, OnWatchChunk 会写入
        ++m_metricWatchReconnect;
        CURLcode res = curl_easy_perform(curl);

        // 说明（issus #20）：bundled libcurl 8.21 对 etcd HTTP gateway 的 chunked watch 流，
        // 收到首个 created 响应后即返回 CURLE_OK（实测 0ms，不挂住；system curl 同请求可挂住）。
        // 因此本循环实际是"快照 resync 轮询"：每轮 range 全量拉取保证路由表新鲜，
        // watch 仅在能挂住时顺带捕获增量事件。真·长连接 watch 见后续 issue。
        // 仍保留 compaction 取消处理：一旦将来 watch 能挂住、被 compaction 取消时，
        // 把起点抬到 compact_revision 之上，避免起点低于 compaction 反复被取消。
        if (m_watchCompactRevision > m_lastRevision)
        {
            ++m_metricWatchCompactCancel;
            ETCD_LOG_INFO("Watch — 被 compaction 取消, 起点 " << m_lastRevision
                          << " → " << m_watchCompactRevision << " 重新同步");
            m_lastRevision = m_watchCompactRevision;
            errBackoffSec = 1;
        }
        else if (res != CURLE_OK)
        {
            // 真·网络错误（etcd 故障/断网）才告警 + 递增退避
            ETCD_LOG_WARN("Watch — 连接异常 url=" << url
                          << " code=" << static_cast<int>(res)
                          << " msg=" << curl_easy_strerror(res));
        }
        else
        {
            errBackoffSec = 1;  // 正常 resync，无需告警（避免历史上的每秒 WARN 风暴）
        }

        if (!m_watchStop)
        {
            const int waitSec = (res == CURLE_OK) ? kWatchResyncIntervalSec : errBackoffSec;
            if (res != CURLE_OK) errBackoffSec = std::min(errBackoffSec * 2, 8);
            std::this_thread::sleep_for(std::chrono::seconds(waitSec));
        }
    }

    ETCD_LOG_INFO("Watch — 线程退出");
}

/*static*/
size_t EtcdCenterConnector::WatchWriteCallback(void* ptr, size_t size,
                                                size_t nmemb, void* userdata)
{
    auto* self = static_cast<EtcdCenterConnector*>(userdata);
    if (!self->m_watchStop)
    {
        self->OnWatchChunk(std::string(static_cast<char*>(ptr), size * nmemb));
    }
    return size * nmemb;
}

void EtcdCenterConnector::OnWatchChunk(const std::string& chunk)
{
    // etcd watch 长连接的响应是每行一个 JSON 对象（JSON lines）
    // 每行格式: {"result":{"events":[...],"created":bool}}
    // 可能一次收到多行
    std::istringstream stream(chunk);
    std::string line;
    bool hasEvents = false;

    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] != '{') continue;

        util::CJsonObject oResp;
        if (!oResp.Parse(line)) continue;

        util::CJsonObject oResult;
        if (!oResp.Get("result", oResult)) continue;

        // 控制信息（created/canceled/compact_revision 均在 result 子对象内）。
        // 旧代码从顶层 oResp 取 created（取不到，恒 false）—— 顺手修正。
        bool created = false, canceled = false;
        oResult.Get("created", created);
        oResult.Get("canceled", canceled);
        if (canceled)
        {
            // 被 etcd compaction 取消：记录 compact_revision，交由 WatchThreadFunc 抬升起点
            // （本回调与 WatchThreadFunc 同在 watch 线程，无需加锁）(issus #20)。
            int64_t cr = 0;
            if (etcd_parse::GetGatewayInt64(oResult, "compact_revision", cr) && cr > 0)
            {
                m_watchCompactRevision = cr;
            }
            continue;
        }
        if (created) continue;  // watch 刚建立，无事件

        util::CJsonObject oEvents;
        if (!oResult.Get("events", oEvents) || !oEvents.IsArray()) continue;
        int evCount = oEvents.GetArraySize();
        for (int i = 0; i < evCount; ++i)
        {
            util::CJsonObject oEv;
            if (!oEvents.Get(i, oEv)) continue;

            std::string evType;
            oEv.Get("type", evType);  // "PUT" / "DELETE"

            util::CJsonObject oKv;
            if (!oEv.Get("kv", oKv)) continue;

            std::string keyB64, valueB64, revStr;
            oKv.Get("key", keyB64);
            oKv.Get("value", valueB64);
            oKv.Get("mod_revision", revStr);

            // 更新 revision（断线续看用）
            if (!revStr.empty())
            {
                int64_t rev = std::stoll(revStr);
                if (rev > m_lastRevision) m_lastRevision = rev;
            }

            WatchEvent wev;
            wev.type  = evType;
            wev.key   = B64Dec(keyB64);
            wev.value = B64Dec(valueB64);

            {
                std::lock_guard<std::mutex> lock(m_watchQueueMutex);
                m_watchQueue.push_back(std::move(wev));
            }
            hasEvents = true;
        }
    }

    // 有事件则唤醒 libev 线程消费
    if (hasEvents)
    {
        ev_async_send(m_loop, &m_watchAsync);
    }
}

/*static*/
void EtcdCenterConnector::WatchAsyncCallback(struct ev_loop* /*loop*/,
                                              ev_async* w,
                                              int /*revents*/)
{
    if (!w || !w->data) return;
    auto* self = static_cast<EtcdCenterConnector*>(w->data);
    self->OnWatchAsync();
}

void EtcdCenterConnector::OnWatchAsync()
{
    // libev 线程执行，消费跨线程队列
    std::vector<WatchEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_watchQueueMutex);
        events.swap(m_watchQueue);
    }

    static const std::string kConfigPrefix("/thunder/config/");

    for (const auto& wev : events)
    {
        // ---- config 变更（Phase 3） ----
        if (wev.key.find(kConfigPrefix) == 0)
        {
            std::string configPath = wev.key.substr(kConfigPrefix.size());
            CenterEvent cev;
            cev.type = CenterEventType::ConfigUpdated;
            if (wev.type == "PUT")
            {
                cev.config_content = wev.value;  // 配置内容
                ETCD_LOG_DEBUG("Watch — CONFIG PUT " << configPath);
            }
            else
            {
                ETCD_LOG_DEBUG("Watch — CONFIG DELETE " << configPath
                               << " (忽略删除事件)");
                continue;  // 配置删除不触发 ConfigUpdated
            }
            m_callback(cev);
            continue;
        }

        // ---- registry 变更（路由，Phase 2） ----
        // key 格式: /thunder/registry/ip:port
        if (wev.key.find(kRegistryPrefix) != 0) continue;

        const std::string ipPort = wev.key.substr(strlen(kRegistryPrefix));
        if (ipPort.rfind(':') == std::string::npos) continue;

        // 维护完整节点表后发"全量"快照 (issus #9 关键):
        //   - etcd v3 grpc-gateway 对 PUT 事件 (EventType=0) 省略 type 字段,只有
        //     DELETE 带 "type":"DELETE";故空 type 当作 PUT。
        //   - 必须每次发全量而非单节点增量:路由 shm 只存最新版本、Worker 把每个
        //     notice 当作"出现类型的完整在线集"来 prune+add,单节点增量会导致
        //     Worker 只认到最后写入的那个节点 → 路由表残缺。
        if (wev.type == "PUT" || wev.type.empty())
        {
            m_nodeRegistry[ipPort] = wev.value;
        }
        else if (wev.type == "DELETE")
        {
            m_nodeRegistry.erase(ipPort);
        }
        else
        {
            continue;
        }

        // 用全表组装完整 NodeNotice (全部走 node_arry_reg, 缺席即下线由 Worker prune)
        NodeNotice notice;
        for (const auto& kv : m_nodeRegistry)
        {
            const std::string& kvIpPort = kv.first;
            auto c = kvIpPort.rfind(':');
            if (c == std::string::npos) continue;
            util::CJsonObject oVal;
            if (!oVal.Parse(kv.second)) continue;

            int32_t  nid  = 0;
            uint32_t wnum = 0;
            std::string ntype;
            oVal.Get("node_id",    nid);
            oVal.Get("node_type",  ntype);
            oVal.Get("worker_num", wnum);

            auto* nr = notice.add_node_arry_reg();
            nr->set_node_ip(kvIpPort.substr(0, c));
            nr->set_node_port(static_cast<uint32_t>(std::stoul(kvIpPort.substr(c + 1))));
            if (nid > 0) nr->set_node_id(static_cast<uint32_t>(nid));
            if (!ntype.empty()) nr->set_node_type(ntype);
            nr->set_worker_num(wnum > 0 ? wnum : 1);  // 缺省至少 1 个 worker, 否则 Worker 不建 identify
        }

        CenterEvent cev;
        cev.type           = CenterEventType::RouteUpdated;
        cev.route_snapshot = notice.SerializeAsString();

        m_callback(cev);
    }
}

} /* namespace net */
