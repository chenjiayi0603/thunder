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
static constexpr const char*    kSlotPrefix        = "/thunder/slot/";
static constexpr const char*    kRegistryPrefix    = "/thunder/registry/";
static constexpr uint32_t       kMaxSlot           = 255;
static constexpr int            kLeaseTTL          = 10;   ///< 秒
static constexpr double         kKeepAliveInterval = 3.0;  ///< 秒

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

    if (is_register)
    {
        // 注册流程：槽位占位 or 幂等续期
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

bool EtcdCenterConnector::QueryRegistry(const std::string& registryKey,
                                         uint32_t& outNodeId)
{
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
    std::string regValue = "{\"node_id\":" + std::to_string(slot) +
        ",\"node_type\":\"" + nodeType + "\"" +
        ",\"node_ip\":\"" + ipPort.substr(0, ipPort.rfind(':')) + "\"" +
        ",\"node_port\":" + ipPort.substr(ipPort.rfind(':') + 1) + "}";

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
    const std::string ipPort      = ip + ":" + std::to_string(port);
    const std::string registryKey = std::string(kRegistryPrefix) + ipPort;

    // ---- Step 1: 幂等查询 —— 已有 registry/ip:port 则续期返回 ----
    uint32_t existingNodeId = 0;
    if (QueryRegistry(registryKey, existingNodeId))
    {
        m_nodeId     = existingNodeId;
        m_registered = true;
        KeepAlive();  // 续租一次，确保租约新鲜
        ETCD_LOG_INFO("DoRegister — 已注册(幂等)，续期 nodeId=" << m_nodeId);
        CenterEvent ev;
        ev.node_id = m_nodeId;
        Emit(CenterEventType::Registered, std::move(ev));
        return;
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
    if (m_leaseId == 0) return;

    if (!m_registered)
    {
        // 注册失败但租约仍存活：主动撤销，避免 etcd 侧资源泄漏
        ETCD_LOG_WARN("OnKeepAliveTimer — 未注册状态发现残留 lease=" << m_leaseId
                      << "，执行 LeaseRevoke");
        LeaseRevoke();
        m_leaseId = 0;
        return;
    }

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

    // 防止 size_t → int 截断溢出（Base64encode_len 参数为 int）
    if (s.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 2))
    {
        ETCD_LOG_WARN("B64 — 输入过大: " << s.size() << " 字节");
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
    const std::string prefix    = kRegistryPrefix;             // "/thunder/registry/"
    std::string       rangeEnd  = prefix;
    if (!rangeEnd.empty())
    {
        // etcd prefix watch: range_end = prefix 的最后一个字节 +1
        rangeEnd.back() = static_cast<char>(static_cast<unsigned char>(rangeEnd.back()) + 1);
    }

    while (!m_watchStop)
    {
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,           0L);    // 无总超时（长连接）
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,     1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,      30L);

        CURLcode res = curl_easy_perform(curl);  // 阻塞直到断线
        ETCD_LOG_WARN("Watch — 连接断开 url=" << url
                      << " code=" << static_cast<int>(res)
                      << " msg=" << curl_easy_strerror(res));

        // 连接断开后短暂等待再重连
        if (!m_watchStop)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
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

        // created=true 表示 watch 刚建立，没有事件，跳过
        bool created = false;
        oResp.Get("created", created);
        if (created) continue;

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

    for (const auto& wev : events)
    {
        // key 格式: /thunder/registry/ip:port
        if (wev.key.find(kRegistryPrefix) != 0) continue;

        const std::string ipPort = wev.key.substr(strlen(kRegistryPrefix));
        auto colon = ipPort.rfind(':');
        if (colon == std::string::npos) continue;

        const std::string ip      = ipPort.substr(0, colon);
        const uint32_t    port    = static_cast<uint32_t>(std::stoul(ipPort.substr(colon + 1)));

        CenterEvent cev;
        cev.type = CenterEventType::RouteUpdated;

        if (wev.type == "PUT")
        {
            // registry value 是 JSON: {"node_id":7,"node_type":"HELLO",...}
            util::CJsonObject oVal;
            oVal.Parse(wev.value);

            int32_t  nid   = 0;
            uint32_t wnum  = 0;
            std::string ntype;
            oVal.Get("node_id",   nid);
            oVal.Get("node_type", ntype);

            NodeNotice notice;
            auto* nr = notice.add_node_arry_reg();
            nr->set_node_ip(ip);
            nr->set_node_port(port);
            if (nid > 0) nr->set_node_id(static_cast<uint32_t>(nid));
            if (!ntype.empty()) nr->set_node_type(ntype);
            // 其他字段（worker_num、access_ip 等）按需扩展
            cev.route_snapshot = notice.SerializeAsString();

            ETCD_LOG_DEBUG("Watch — PUT " << ipPort
                           << " nodeId=" << nid << " type=" << ntype);
        }
        else if (wev.type == "DELETE")
        {
            // 节点下线：key 被删除（租约过期 / 主动注销）
            NodeNotice notice;
            auto* nr = notice.add_node_arry_exit();
            nr->set_node_ip(ip);
            nr->set_node_port(port);
            cev.route_snapshot = notice.SerializeAsString();

            ETCD_LOG_DEBUG("Watch — DELETE " << ipPort);
        }

        m_callback(cev);
    }
}

} /* namespace net */
