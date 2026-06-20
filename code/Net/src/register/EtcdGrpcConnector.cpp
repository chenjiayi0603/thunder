/*******************************************************************************
 * Project:  Net
 * @file     EtcdGrpcConnector.cpp
 * @brief    CenterConnector 的 gRPC 实现（Watch 事件驱动）
 ******************************************************************************/
#include "EtcdGrpcConnector.hpp"

#include <etcd/SyncClient.hpp>
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/v3/Transaction.hpp>

#include "protocol/oss_sys.pb.h"
#include "util/json/CJsonObject.hpp"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

#include <chrono>

static auto sLogger = log4cplus::Logger::getInstance("etcd-grpc");
#define GLOG_INFO(msg)  LOG4CPLUS_INFO(sLogger,  msg)
#define GLOG_WARN(msg)  LOG4CPLUS_WARN(sLogger,  msg)
#define GLOG_ERROR(msg) LOG4CPLUS_ERROR(sLogger, msg)

static constexpr int      kLeaseTTL         = 30;   // seconds
static constexpr int      kKeepAliveRefresh = 10;   // unary keepalive interval
static constexpr int      kPollInterval     = 5;    // config poll interval
static constexpr uint32_t kMaxSlot          = 255;
static constexpr const char* kRegistryPrefix = "/thunder/registry/";
static constexpr const char* kConfigPrefix   = "/thunder/config/";

namespace net
{

// ============================================================
// 构造 / 析构
// ============================================================

EtcdGrpcConnector::EtcdGrpcConnector(const util::CJsonObject& conf)
    : m_oConf(conf)
{}

EtcdGrpcConnector::~EtcdGrpcConnector()
{
    if (m_asyncStarted && m_loop)
    {
        ev_async_stop(m_loop, &m_async);
        m_asyncStarted = false;
    }
}

// ============================================================
// Init
// ============================================================

bool EtcdGrpcConnector::Init(struct ev_loop* loop, CenterEventCallback cb, void* user_data)
{
    m_loop     = loop;
    m_callback = std::move(cb);
    m_userData = user_data;

    std::string endpoints;
    m_oConf.Get("etcd_endpoints", endpoints);
    if (endpoints.empty())
    {
        GLOG_ERROR("EtcdGrpcConnector::Init — etcd_endpoints not configured");
        return false;
    }
    auto comma = endpoints.find(',');
    m_endpoint = (comma != std::string::npos) ? endpoints.substr(0, comma) : endpoints;
    while (!m_endpoint.empty() && m_endpoint.front() == ' ') m_endpoint.erase(0, 1);
    while (!m_endpoint.empty() && m_endpoint.back()  == ' ') m_endpoint.pop_back();
    if (m_endpoint.empty())
    {
        GLOG_ERROR("EtcdGrpcConnector::Init — no valid endpoint");
        return false;
    }

    ev_async_init(&m_async, AsyncCb);
    m_async.data = this;
    ev_async_start(m_loop, &m_async);
    m_asyncStarted = true;

    m_stopFlag   = false;
    m_grpcThread = std::thread([this] { GrpcThreadMain(); });

    GLOG_INFO("EtcdGrpcConnector::Init — endpoint=" << m_endpoint);
    return true;
}

// ============================================================
// Destroy
// ============================================================

void EtcdGrpcConnector::Destroy()
{
    PostCmd({CmdType::Stop});
    if (m_grpcThread.joinable())
        m_grpcThread.join();

    if (m_asyncStarted && m_loop)
    {
        ev_async_stop(m_loop, &m_async);
        m_asyncStarted = false;
    }
    m_registered = false;
}

// ============================================================
// ReportNodeStatus（libev 线程）
// ============================================================

bool EtcdGrpcConnector::ReportNodeStatus(const std::string& node_report, bool is_register)
{
    NodeReport report;
    if (!report.ParseFromString(node_report))
    {
        GLOG_ERROR("EtcdGrpcConnector::ReportNodeStatus — parse failed");
        return false;
    }
    const std::string& ip   = report.node_ip();
    const uint32_t     port = report.node_port();
    const std::string& type = report.node_type();
    const uint32_t     wnum = report.worker_num();

    if (ip.empty() || port == 0)
    {
        GLOG_ERROR("EtcdGrpcConnector::ReportNodeStatus — empty ip/port");
        return false;
    }
    if (is_register || !m_registered.load())
        PostCmd({CmdType::Register, ip, port, type, wnum});
    return true;
}

// ============================================================
// PutConfig（libev 线程 → gRPC 线程；B3）
// ============================================================

void EtcdGrpcConnector::PutConfig(const std::string& key, const std::string& value)
{
    Cmd cmd{};
    cmd.type        = CmdType::PutConfig;
    cmd.configKey   = key;
    cmd.configValue = value;
    PostCmd(std::move(cmd));
}

// ============================================================
// ev_async 回调（libev 线程）
// ============================================================

void EtcdGrpcConnector::AsyncCb(struct ev_loop*, struct ev_async* w, int)
{
    static_cast<EtcdGrpcConnector*>(w->data)->OnAsync();
}

void EtcdGrpcConnector::OnAsync()
{
    std::queue<CenterEvent> q;
    {
        std::lock_guard<std::mutex> lk(m_eventMutex);
        std::swap(q, m_eventQueue);
    }
    while (!q.empty())
    {
        m_callback(q.front());
        q.pop();
    }
}

// ============================================================
// 事件推送（任意线程 → libev 线程）
// ============================================================

void EtcdGrpcConnector::PushEvent(CenterEvent ev)
{
    {
        std::lock_guard<std::mutex> lk(m_eventMutex);
        m_eventQueue.push(std::move(ev));
    }
    ev_async_send(m_loop, &m_async);
}

// ============================================================
// 命令投递（libev 线程 → gRPC 线程）
// ============================================================

void EtcdGrpcConnector::PostCmd(Cmd cmd)
{
    {
        std::lock_guard<std::mutex> lk(m_cmdMutex);
        m_cmdQueue.push(std::move(cmd));
    }
    m_cmdCv.notify_one();
}

// ============================================================
// gRPC 线程主循环
// ============================================================

void EtcdGrpcConnector::GrpcThreadMain()
{
    // etcdClient 必须比 m_watcher 活得长：
    // Stop 时先 Cancel/reset m_watcher（join Watcher::task_），再让 etcdClient 析构。
    etcd::SyncClient etcdClient(m_endpoint);

    auto cancelWatcher = [this] {
        if (m_watcher)
        {
            m_watcher->Cancel();
            m_watcher.reset();
        }
    };

    try
    {

    GLOG_INFO("GrpcThread: connected to " << m_endpoint);

    etcd::Response leaseResp = etcdClient.leasegrant(kLeaseTTL);
    if (leaseResp.error_code() != 0)
    {
        GLOG_ERROR("GrpcThread: leasegrant failed: " << leaseResp.error_message());
        CenterEvent ev{};
        ev.type    = CenterEventType::ConnectionLost;
        ev.errcode = leaseResp.error_code();
        ev.errmsg  = leaseResp.error_message();
        PushEvent(std::move(ev));
        return;
    }
    m_leaseId = leaseResp.value().lease();
    GLOG_INFO("GrpcThread: lease id=" << m_leaseId);

    using Clock    = std::chrono::steady_clock;
    using Seconds  = std::chrono::seconds;

    auto lastKeepalive   = Clock::now();
    auto lastPoll        = Clock::now();
    const auto kWaitTick = Seconds(1);

    while (true)
    {
        std::unique_lock<std::mutex> lk(m_cmdMutex);
        m_cmdCv.wait_for(lk, kWaitTick, [this] { return !m_cmdQueue.empty(); });

        while (!m_cmdQueue.empty())
        {
            Cmd cmd = std::move(m_cmdQueue.front());
            m_cmdQueue.pop();
            lk.unlock();

            if (cmd.type == CmdType::Stop)
            {
                GLOG_INFO("GrpcThread: received Stop");
                cancelWatcher();   // join Watcher::task_ before etcdClient destroyed
                if (m_leaseId)
                {
                    etcdClient.leaserevoke(m_leaseId);
                    m_leaseId = 0;
                }
                return;
            }

            if (cmd.type == CmdType::PutConfig)
            {
                auto r = etcdClient.put(cmd.configKey, cmd.configValue, m_leaseId);
                if (r.error_code() != 0)
                    GLOG_WARN("GrpcThread: PutConfig failed key=" << cmd.configKey
                              << " err=" << r.error_message());
            }
            else if (cmd.type == CmdType::Register)
            {
                const bool ok = DoRegisterGrpc(etcdClient,
                                               cmd.nodeIp, cmd.nodePort,
                                               cmd.nodeType, cmd.workerNum);
                CenterEvent ev{};
                ev.type = CenterEventType::Registered;
                if (ok)
                {
                    m_registered  = true;
                    m_myNodeType  = cmd.nodeType;
                    m_myNodeIp    = cmd.nodeIp;
                    m_myNodePort  = cmd.nodePort;
                    m_myWorkerNum = cmd.workerNum;
                    ev.node_id    = m_nodeId;
                    GLOG_INFO("GrpcThread: registered node_id=" << m_nodeId);
                    DoInitialSnapshot(etcdClient);
                    DoStartWatch(etcdClient);
                    lastPoll = Clock::now();
                }
                else
                {
                    ev.errcode = -1;
                    ev.errmsg  = "slot txn failed";
                    GLOG_ERROR("GrpcThread: registration failed");
                }
                PushEvent(std::move(ev));
            }

            lk.lock();
        }
        lk.unlock();

        auto now = Clock::now();

        // Watch 断流后重建：先 join 旧 Watcher，再快照，再启动新 Watcher
        if (m_watchEnded.load() && m_registered.load())
        {
            GLOG_WARN("GrpcThread: Watch ended, rebuilding snapshot + Watch");
            m_watchEnded = false;
            m_watcher.reset();   // join task_，确保 OnWatchEvent 不再并发
            DoInitialSnapshot(etcdClient);
            DoStartWatch(etcdClient);
        }

        if (m_leaseId && now - lastKeepalive >= Seconds(kKeepAliveRefresh))
        {
            DoKeepalive(etcdClient);
            lastKeepalive = now;
        }

        if (m_registered.load() && now - lastPoll >= Seconds(kPollInterval))
        {
            DoPollConfig(etcdClient);
            lastPoll = now;
        }
    }

    } // try
    catch (const std::exception& e)
    {
        cancelWatcher();   // join Watcher::task_ before etcdClient goes out of scope
        GLOG_ERROR("GrpcThread: uncaught exception: " << e.what());
        CenterEvent ev{};
        ev.type   = CenterEventType::ConnectionLost;
        ev.errmsg = e.what();
        PushEvent(std::move(ev));
    }
}

// ============================================================
// 注册逻辑（gRPC 线程）
// ============================================================

bool EtcdGrpcConnector::DoRegisterGrpc(etcd::SyncClient& client,
                                         const std::string& ip, uint32_t port,
                                         const std::string& nodeType, uint32_t workerNum)
{
    const std::string ipPort      = ip + ":" + std::to_string(port);
    const std::string registryKey = BuildRegistryKey(nodeType, ip, port);

    auto getResp = client.get(registryKey);
    if (getResp.error_code() == 0 && !getResp.value().as_string().empty())
    {
        util::CJsonObject oVal;
        uint32_t nid = 0;
        if (oVal.Parse(getResp.value().as_string()))
            oVal.Get("node_id", nid);
        if (nid > 0)
        {
            const std::string slotKey  = SlotKey(static_cast<int>(nid));
            const std::string regValue = BuildRegistryValue(nid, nodeType, ip, port, workerNum);
            const auto s = client.put(slotKey,     ipPort,   m_leaseId);
            const auto r = client.put(registryKey, regValue, m_leaseId);
            if (s.error_code() == 0 && r.error_code() == 0)
            {
                m_nodeId = nid;
                GLOG_INFO("DoRegisterGrpc: rebind existing node_id=" << nid);
                return true;
            }
            GLOG_WARN("DoRegisterGrpc: rebind failed, fall through to slot scan");
        }
    }

    const uint32_t startSlot = static_cast<uint32_t>(
        std::hash<std::string>{}(ipPort) % kMaxSlot) + 1;

    for (uint32_t i = 0; i < kMaxSlot; ++i)
    {
        const int         slot     = static_cast<int>(((startSlot - 1 + i) % kMaxSlot) + 1);
        const std::string slotKey  = SlotKey(slot);
        const std::string regValue = BuildRegistryValue(
            static_cast<uint32_t>(slot), nodeType, ip, port, workerNum);

        etcdv3::Transaction txn;
        txn.add_compare_create(slotKey, etcdv3::CompareResult::EQUAL, 0);
        txn.add_success_put(slotKey,     ipPort,   m_leaseId);
        txn.add_success_put(registryKey, regValue, m_leaseId);
        txn.add_failure_range(slotKey);

        auto txnResp = client.txn(txn);
        if (txnResp.error_code() == 0 && txnResp.is_ok())
        {
            m_nodeId = static_cast<uint32_t>(slot);
            GLOG_INFO("DoRegisterGrpc: claimed slot=" << slot);
            return true;
        }
    }

    GLOG_ERROR("DoRegisterGrpc: all " << kMaxSlot << " slots occupied");
    return false;
}

// ============================================================
// 初始路由快照（gRPC 线程）
// 调用时 m_watcher 为 nullptr（首次注册或 reset() 后），无并发写 m_nodeRegistry。
// ============================================================

void EtcdGrpcConnector::DoInitialSnapshot(etcd::SyncClient& client)
{
    auto resp = client.ls(kRegistryPrefix);
    if (resp.error_code() != 0)
    {
        GLOG_WARN("DoInitialSnapshot: ls failed: " << resp.error_message());
        return;
    }

    m_watchRevision = resp.index();   // Watch 从此 revision+1 开始，不漏事件

    {
        std::lock_guard<std::mutex> lk(m_registryMutex);
        m_nodeRegistry.clear();
        for (const auto& v : resp.values())
        {
            const std::string& fullKey = v.key();
            if (fullKey.size() <= strlen(kRegistryPrefix)) continue;
            std::string rest  = fullKey.substr(strlen(kRegistryPrefix));
            auto        slash = rest.find('/');
            if (slash == std::string::npos) continue;
            std::string ipPort = rest.substr(slash + 1);
            if (ipPort.rfind(':') == std::string::npos) continue;
            m_nodeRegistry[ipPort] = v.as_string();
        }
        GLOG_INFO("DoInitialSnapshot: " << m_nodeRegistry.size()
                  << " nodes, revision=" << m_watchRevision);
        AssembleAndPushRouteUpdated();
    }
}

// ============================================================
// 启动 Watch（gRPC 线程，DoInitialSnapshot 之后调用）
// ============================================================

void EtcdGrpcConnector::DoStartWatch(etcd::SyncClient& client)
{
    m_watchEnded = false;
    // fromIndex = m_watchRevision + 1：快照已包含 revision 及之前的状态，Watch 从下一个事件开始
    m_watcher = std::make_unique<etcd::Watcher>(
        client,
        kRegistryPrefix,
        m_watchRevision + 1,
        [this](etcd::Response resp) { OnWatchEvent(std::move(resp)); },
        [this](bool cancelled)      { OnWatchEnded(cancelled); },
        true   // recursive: 匹配 kRegistryPrefix 下所有 key
    );
    GLOG_INFO("DoStartWatch: watching " << kRegistryPrefix
              << " from revision=" << (m_watchRevision + 1));
}

// ============================================================
// Watch 事件回调（Watcher 内部线程）
// ============================================================

void EtcdGrpcConnector::OnWatchEvent(etcd::Response resp)
{
    if (resp.error_code() != 0)
    {
        GLOG_WARN("OnWatchEvent: error " << resp.error_code()
                  << " " << resp.error_message());
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_registryMutex);
        for (const auto& ev : resp.events())
        {
            if (!ev.has_kv()) continue;
            const std::string& fullKey = ev.kv().key();
            if (fullKey.size() <= strlen(kRegistryPrefix)) continue;
            std::string rest  = fullKey.substr(strlen(kRegistryPrefix));
            auto        slash = rest.find('/');
            if (slash == std::string::npos) continue;
            std::string ipPort = rest.substr(slash + 1);
            if (ipPort.rfind(':') == std::string::npos) continue;

            if (ev.event_type() == etcd::Event::EventType::PUT)
            {
                m_nodeRegistry[ipPort] = ev.kv().as_string();
                changed = true;
                GLOG_INFO("OnWatchEvent: PUT " << ipPort);
            }
            else if (ev.event_type() == etcd::Event::EventType::DELETE_)
            {
                m_nodeRegistry.erase(ipPort);
                changed = true;
                GLOG_INFO("OnWatchEvent: DELETE " << ipPort);
            }
        }

        if (changed)
            AssembleAndPushRouteUpdated();
    }
}

// ============================================================
// Watch 结束回调（Watcher 内部线程，etcd 断连或主动 Cancel 时触发）
// ============================================================

void EtcdGrpcConnector::OnWatchEnded(bool cancelled)
{
    if (cancelled)
    {
        GLOG_INFO("OnWatchEnded: Watch normally cancelled (Stop path)");
        return;   // Stop 路径：GrpcThreadMain 主动 Cancel，不需要重建
    }
    GLOG_WARN("OnWatchEnded: Watch stream ended unexpectedly, will rebuild");
    m_watchEnded = true;
    // 通过 m_cmdCv 唤醒 GrpcThreadMain（避免等满 1s tick）
    m_cmdCv.notify_one();
}

// ============================================================
// 租约续约（gRPC 线程，unary）
// ============================================================

void EtcdGrpcConnector::DoKeepalive(etcd::SyncClient& client)
{
    auto ttlResp = client.leasetimetolive(m_leaseId);
    if (ttlResp.error_code() != 0)
    {
        GLOG_WARN("DoKeepalive: leasetimetolive failed: " << ttlResp.error_message());
        return;
    }

    const int64_t remainTTL = ttlResp.value().ttl();
    GLOG_INFO("DoKeepalive: lease=" << m_leaseId << " remainTTL=" << remainTTL);

    if (remainTTL > kLeaseTTL / 2)
        return;

    auto newLease = client.leasegrant(kLeaseTTL);
    if (newLease.error_code() != 0)
    {
        GLOG_ERROR("DoKeepalive: leasegrant failed: " << newLease.error_message());
        return;
    }

    const int64_t     newLeaseId = newLease.value().lease();
    const std::string slotKey    = SlotKey(static_cast<int>(m_nodeId));
    const std::string regKey     = BuildRegistryKey(m_myNodeType, m_myNodeIp, m_myNodePort);
    const std::string ipPort     = m_myNodeIp + ":" + std::to_string(m_myNodePort);
    const std::string regValue   = BuildRegistryValue(m_nodeId, m_myNodeType,
                                                       m_myNodeIp, m_myNodePort, m_myWorkerNum);

    client.put(slotKey, ipPort,   newLeaseId);
    client.put(regKey,  regValue, newLeaseId);
    client.leaserevoke(m_leaseId);
    m_leaseId = newLeaseId;
    GLOG_INFO("DoKeepalive: re-granted lease=" << m_leaseId);
}

// ============================================================
// 配置轮询（gRPC 线程，unary get）
// ============================================================

void EtcdGrpcConnector::DoPollConfig(etcd::SyncClient& client)
{
    if (m_myNodeType.empty()) return;

    const std::string cfgKey = std::string(kConfigPrefix) + "module/" + m_myNodeType;
    auto resp = client.get(cfgKey);
    if (resp.error_code() != 0) return;

    const std::string val = resp.value().as_string();
    if (val == m_lastConfigValue) return;
    m_lastConfigValue = val;

    CenterEvent cev{};
    cev.type           = CenterEventType::ConfigUpdated;
    cev.config_content = val;
    PushEvent(std::move(cev));
    GLOG_INFO("DoPollConfig: ConfigUpdated key=" << cfgKey);
}

// ============================================================
// 组装路由快照并推送（调用方须持有 m_registryMutex）
// ============================================================

void EtcdGrpcConnector::AssembleAndPushRouteUpdated()
{
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
        oVal.Get("worker_num", wnum);
        oVal.Get("node_type",  ntype);

        auto* nr = notice.add_node_arry_reg();
        nr->set_node_ip(kvIpPort.substr(0, c));
        nr->set_node_port(static_cast<uint32_t>(std::stoul(kvIpPort.substr(c + 1))));
        if (nid  > 0)       nr->set_node_id(static_cast<uint32_t>(nid));
        if (!ntype.empty()) nr->set_node_type(ntype);
        nr->set_worker_num(wnum > 0 ? wnum : 1);
    }

    CenterEvent cev{};
    cev.type           = CenterEventType::RouteUpdated;
    cev.route_snapshot = notice.SerializeAsString();
    PushEvent(std::move(cev));
}

// ============================================================
// 工具函数
// ============================================================

std::string EtcdGrpcConnector::BuildRegistryKey(const std::string& nodeType,
                                                  const std::string& ip, uint32_t port)
{
    return "/thunder/registry/" + nodeType + "/" + ip + ":" + std::to_string(port);
}

std::string EtcdGrpcConnector::BuildRegistryValue(uint32_t nodeId, const std::string& nodeType,
                                                    const std::string& ip, uint32_t port,
                                                    uint32_t workerNum)
{
    return "{\"node_id\":"     + std::to_string(nodeId)    +
           ",\"node_type\":\"" + nodeType                  + "\"" +
           ",\"node_ip\":\""   + ip                        + "\"" +
           ",\"node_port\":"   + std::to_string(port)      +
           ",\"worker_num\":"  + std::to_string(workerNum > 0 ? workerNum : 1) + "}";
}

std::string EtcdGrpcConnector::SlotKey(int slot)
{
    return "/thunder/slot/" + std::to_string(slot);
}

} /* namespace net */
