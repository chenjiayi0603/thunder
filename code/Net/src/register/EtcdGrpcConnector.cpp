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
#include <cstdlib>

static auto sLogger = log4cplus::Logger::getInstance("etcd-grpc");
#define GLOG_INFO(msg)  LOG4CPLUS_INFO(sLogger,  msg)
#define GLOG_WARN(msg)  LOG4CPLUS_WARN(sLogger,  msg)
#define GLOG_ERROR(msg) LOG4CPLUS_ERROR(sLogger, msg)

static constexpr int      kLeaseTTL           = 60;   // seconds (give more tolerance)
static constexpr int      kKeepAliveRefresh   = 10;   // active keepalive interval (leasekeepalive every 10s, far below 60s TTL)
static constexpr int      kPollInterval         = 5;    // config poll interval
static constexpr int      kCanaryPollInterval   = 30;   // canary fallback check interval
static constexpr int      kWatchHealthTimeout   = 45;   // 没有 Watch 事件的最大容忍时间
static constexpr int64_t  kWatchRebuildBackoff  = 5;    // 两次重建之间的最小间隔（防抖）
static constexpr int      kZombieMaxAge       = 60;   // skip entries older than 2× lease TTL
static constexpr uint32_t kMaxSlot            = 255;
static constexpr const char* kRegistryPrefix = "/thunder/registry/";
static constexpr const char* kCanaryPrefix   = "/thunder/canary/";
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

    // 读 NODE_VERSION 环境变量（用于灰度权重分组）
    const char* nodeVer = std::getenv("NODE_VERSION");
    m_myNodeVersion = nodeVer ? nodeVer : "v1";

    GLOG_INFO("EtcdGrpcConnector::Init — endpoint=" << m_endpoint
              << " node_version=" << m_myNodeVersion);
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
    auto cancelWatchers = [this] {
        if (m_registryWatcher) { m_registryWatcher->Cancel(); m_registryWatcher.reset(); }
        if (m_canaryWatcher)   { m_canaryWatcher->Cancel();   m_canaryWatcher.reset();   }
    };

    // #160: 外层重连循环 — gRPC 连接断开/异常后自动重试
    int  retryDelaySec = 1;
    const int maxRetryDelaySec = 30;
    while (m_running)
    {
        // etcdClient 必须比 m_watcher 活得长：
        // Stop 时先 Cancel/reset m_watcher（join Watcher::task_），再让 etcdClient 析构。
        etcd::SyncClient etcdClient(m_endpoint);

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
            goto reconnect;  // break to retry loop
        }
        m_leaseId = leaseResp.value().lease();
        GLOG_INFO("GrpcThread: lease id=" << m_leaseId);

        using Clock    = std::chrono::steady_clock;
        using Seconds  = std::chrono::seconds;

        auto lastKeepalive   = Clock::now();
        auto lastPoll        = Clock::now();
        auto lastCanaryPoll  = Clock::now();
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
                if (m_leaseId)
                {
                    etcdClient.leaserevoke(m_leaseId);
                    m_leaseId = 0;
                }
                cancelWatchers();
                m_running = false;
                return;  // 正常退出，不走重连循环
            }

            if (cmd.type == CmdType::PutConfig)
            {
                // Write without lease — config is desired state, not tied to pod lifecycle.
                // Only seed if key does NOT exist (create_revision == 0),
                // so admin-deployed config is never overwritten.
                etcdv3::Transaction txn;
                txn.add_compare_create(cmd.configKey, etcdv3::CompareResult::EQUAL, 0);
                txn.add_success_put(cmd.configKey, cmd.configValue);
                txn.add_failure_range(cmd.configKey);
                auto r = etcdClient.txn(txn);
                if (r.error_code() != 0 || !r.is_ok())
                    GLOG_INFO("PutConfig: key exists, skip seed (key=" << cmd.configKey << ")");
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
                    GLOG_INFO("GrpcThread: registered node_id=" << m_nodeId
                              << " version=" << m_myNodeVersion);
                    DoInitialSnapshot(etcdClient);
                    DoStartWatch(etcdClient);
                    DoCanarySnapshot(etcdClient);
                    DoStartCanaryWatch(etcdClient);
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

        // ---- Watch 重建（三种触发条件） ----
        // 1. m_watchEnded==true（OnWatchEnded 回调触发）
        // 2. 距上次 Watch 事件超过 kWatchHealthTimeout（静默 hang 检测）
        if (m_registered.load())
        {
            int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();

            bool needRebuild = false;
            if (m_watchEnded.load())
            {
                needRebuild = true;
                GLOG_WARN("GrpcThread: Watch ended callback received, rebuilding...");
            }
            else if (m_lastWatchEventSec.load() > 0 &&
                     nowSec - m_lastWatchEventSec.load() > kWatchHealthTimeout)
            {
                needRebuild = true;
                GLOG_WARN("GrpcThread: Watch silent too long ("
                          << (nowSec - m_lastWatchEventSec.load())
                          << "s > " << kWatchHealthTimeout
                          << "s), health-check rebuild...");
            }

            if (needRebuild)
            {
                // 防抖：距上次重建至少间隔 kWatchRebuildBackoff 秒
                if (m_lastWatchRebuildSec > 0 &&
                    nowSec - m_lastWatchRebuildSec < kWatchRebuildBackoff)
                {
                    // 不重建，等下一个 tick
                }
                else
                {
                    m_lastWatchRebuildSec = nowSec;
                    const bool fromHealthCheck = !m_watchEnded.load();

                    // ---- 清理旧 Watcher ----
                    //   OnWatchEnded 路径：线程已正常退出，reset() 安全 join
                    //   健康检查路径：线程可能 hung 在 cq_.Next()，
                    //     reset()→~Watcher()→Cancel()→task_.join() 会永久阻塞
                    //     故用 release() 分离旧 Watcher，接受轻微内存泄漏
                    if (fromHealthCheck)
                    {
                        m_registryWatcher.release();
                        m_canaryWatcher.release();
                    }
                    else
                    {
                        m_registryWatcher.reset();
                        m_canaryWatcher.reset();
                    }
                    m_watchEnded = false;

                    try
                    {
                        DoInitialSnapshot(etcdClient);
                        DoStartWatch(etcdClient);
                        DoCanarySnapshot(etcdClient);
                        DoStartCanaryWatch(etcdClient);
                        // 重建成功后重置健康检查计时器
                        m_lastWatchEventSec = nowSec;
                        GLOG_INFO("GrpcThread: Watch rebuild complete");
                    }
                    catch (const std::exception& e)
                    {
                        GLOG_ERROR("GrpcThread: Watch rebuild failed: " << e.what()
                                   << ", will retry next tick (backoff "
                                   << kWatchRebuildBackoff << "s)");
                        // 不重抛——保持 gRPC 线程存活，下次 tick 继续重试
                    }
                }
            }
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

        // 30s 兜底：主动检查 canary 权重是否变更（Watch 可能丢事件）
        if (m_registered.load() && now - lastCanaryPoll >= Seconds(kCanaryPollInterval))
        {
            auto cresp = etcdClient.ls(kCanaryPrefix);
            if (cresp.error_code() == 0 && cresp.index() != m_canaryWatchRevision)
            {
                GLOG_INFO("GrpcThread: canary fallback — revision changed "
                          << m_canaryWatchRevision << " -> " << cresp.index());
                DoCanarySnapshot(etcdClient);
                {
                    std::lock_guard<std::mutex> lk(m_registryMutex);
                    AssembleAndPushRouteUpdated();
                }
            }
            lastCanaryPoll = now;
        }
    }

        } // try
        catch (const std::exception& e)
        {
            cancelWatchers();
            GLOG_ERROR("GrpcThread: uncaught exception: " << e.what()
                       << " — will reconnect after backoff");
            CenterEvent ev{};
            ev.type   = CenterEventType::ConnectionLost;
            ev.errmsg = e.what();
            PushEvent(std::move(ev));
        }

    reconnect:
        // #160: 指数退避重连 (1s → 2s → 4s → ... → 30s max)
        if (!m_running) break;
        GLOG_INFO("GrpcThread: reconnecting in " << retryDelaySec << "s...");
        std::this_thread::sleep_for(std::chrono::seconds(retryDelaySec));
        if (retryDelaySec < maxRetryDelaySec)
            retryDelaySec *= 2;
    } // while (m_running)
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
            const std::string regValue = BuildRegistryValue(nid, nodeType, ip, port, workerNum, m_myNodeVersion);
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
            static_cast<uint32_t>(slot), nodeType, ip, port, workerNum, m_myNodeVersion);

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
    m_registryWatcher = std::make_unique<etcd::Watcher>(
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
    // 记录最后事件时间用于 Watch 健康检查
    m_lastWatchEventSec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

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
// Canary Watch 结束回调（Watcher 内部线程）
// 独立于 registry watcher，canary 断流不影响 registry watch
// ============================================================

void EtcdGrpcConnector::OnCanaryWatchEnded(bool cancelled)
{
    if (cancelled)
    {
        GLOG_INFO("OnCanaryWatchEnded: Canary Watch normally cancelled");
        return;
    }
    GLOG_WARN("OnCanaryWatchEnded: Canary Watch stream ended unexpectedly");
    // canary 断流：只标记需要全量重建（因为重建时两个 watcher 都会重建）
    m_watchEnded = true;
    m_cmdCv.notify_one();
}

// ============================================================
// 租约续约（gRPC 线程，unary）
// ============================================================

void EtcdGrpcConnector::DoKeepalive(etcd::SyncClient& client)
{
    // #160: 增强 keepalive 健壮性
    //   leasetimetolive 查询 lease 是否存活:
    //   - 成功 + TTL OK    → 打印日志, 不做操作
    //   - 成功 + TTL < 半  → grant 新 lease + 重新 PUT key (旧逻辑, 守护)
    //   - 失败             → lease 丢失, 自动重新注册 (新逻辑)
    auto ttlResp = client.leasetimetolive(m_leaseId);
    if (ttlResp.error_code() != 0)
    {
        GLOG_WARN("DoKeepalive: lease " << m_leaseId
                  << " lost (" << ttlResp.error_message() << "), re-registering...");

        auto newLease = client.leasegrant(kLeaseTTL);
        if (newLease.error_code() != 0)
        {
            GLOG_ERROR("DoKeepalive: re-register leasegrant failed: "
                       << newLease.error_message());
            return;
        }

        const int64_t     newLeaseId = newLease.value().lease();
        const std::string slotKey    = SlotKey(static_cast<int>(m_nodeId));
        const std::string regKey     = BuildRegistryKey(m_myNodeType, m_myNodeIp, m_myNodePort);
        const std::string ipPort     = m_myNodeIp + ":" + std::to_string(m_myNodePort);
        const std::string regValue   = BuildRegistryValue(m_nodeId, m_myNodeType,
                                                           m_myNodeIp, m_myNodePort, m_myWorkerNum, m_myNodeVersion);

        client.put(slotKey, ipPort,   newLeaseId);
        client.put(regKey,  regValue, newLeaseId);
        m_leaseId = newLeaseId;

        GLOG_INFO("DoKeepalive: re-registered lease=" << m_leaseId
                  << " type=" << m_myNodeType << " addr=" << ipPort);
        return;
    }

    const int64_t remainTTL = ttlResp.value().ttl();
    GLOG_INFO("DoKeepalive: lease=" << m_leaseId << " ttl=" << remainTTL);

    if (remainTTL > kLeaseTTL / 2)
        return;

    // TTL 低于一半时换新 lease（旧守护逻辑, 防止边界条件）
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
                                                       m_myNodeIp, m_myNodePort, m_myWorkerNum, m_myNodeVersion);

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
// Canary 权重快照（gRPC 线程）
// ============================================================

void EtcdGrpcConnector::DoCanarySnapshot(etcd::SyncClient& client)
{
    auto resp = client.ls(kCanaryPrefix);
    if (resp.error_code() != 0)
    {
        GLOG_INFO("DoCanarySnapshot: no canary keys (err=" << resp.error_message() << ")");
        return;
    }

    m_canaryWatchRevision = resp.index();

    {
        std::lock_guard<std::mutex> lk(m_canaryMutex);
        m_canaryWeights.clear();
        for (const auto& v : resp.values())
        {
            const std::string& key = v.key();
            if (key.size() <= strlen(kCanaryPrefix)) continue;
            // key: /thunder/canary/{NODE_TYPE}/weights
            std::string rest = key.substr(strlen(kCanaryPrefix));
            auto slash = rest.rfind('/');
            if (slash == std::string::npos) continue;
            std::string nodeType = rest.substr(0, slash);
            std::string raw = v.as_string();
            // #137 防御: 校验 JSON 合法性, 跳过脏数据避免 Worker SIGSEGV
            if (raw.size() < 4 || raw[0] != '{' || raw.find("\\\"") != std::string::npos)
            {
                GLOG_WARN("DoCanarySnapshot: skip invalid JSON for " << nodeType
                          << " (len=" << raw.size() << "): " << raw);
                continue;
            }
            m_canaryWeights[nodeType] = std::move(raw);
        }
        GLOG_INFO("DoCanarySnapshot: " << m_canaryWeights.size()
                  << " canary entries, revision=" << m_canaryWatchRevision);
    }
}

// ============================================================
// 启动 Canary Watch
// ============================================================

void EtcdGrpcConnector::DoStartCanaryWatch(etcd::SyncClient& client)
{
    m_canaryWatcher = std::make_unique<etcd::Watcher>(
        client,
        kCanaryPrefix,
        m_canaryWatchRevision > 0 ? m_canaryWatchRevision + 1 : 0,
        [this](etcd::Response resp) {
            if (resp.error_code() != 0) {
                GLOG_WARN("CanaryWatch event error: " << resp.error_code() << " " << resp.error_message());
                return;
            }
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(m_canaryMutex);
                for (const auto& ev : resp.events())
                {
                    if (!ev.has_kv()) continue;
                    const std::string& key = ev.kv().key();
                    if (key.size() <= strlen(kCanaryPrefix)) continue;
                    std::string rest = key.substr(strlen(kCanaryPrefix));
                    auto slash = rest.rfind('/');
                    if (slash == std::string::npos) continue;
                    std::string nodeType = rest.substr(0, slash);

                    if (ev.event_type() == etcd::Event::EventType::PUT)
                    {
                        std::string raw = ev.kv().as_string();
                        // #137 防御: 校验 JSON 合法性
                        if (raw.size() < 4 || raw[0] != '{' || raw.find("\\\"") != std::string::npos)
                        {
                            GLOG_WARN("CanaryWatch: skip invalid JSON for " << nodeType
                                      << " (len=" << raw.size() << "): " << raw);
                            continue;
                        }
                        m_canaryWeights[nodeType] = std::move(raw);
                        changed = true;
                        GLOG_INFO("CanaryWatch: PUT " << nodeType << " = " << m_canaryWeights[nodeType]);
                    }
                    else if (ev.event_type() == etcd::Event::EventType::DELETE_)
                    {
                        m_canaryWeights.erase(nodeType);
                        changed = true;
                        GLOG_INFO("CanaryWatch: DELETE " << nodeType);
                    }
                }
            }
            if (changed)
            {
                // Canary 权重重载整个路由表
                std::lock_guard<std::mutex> lk(m_registryMutex);
                AssembleAndPushRouteUpdated();
            }
        },
        [this](bool cancelled) { OnCanaryWatchEnded(cancelled); },
        true   // recursive
    );
    GLOG_INFO("DoStartCanaryWatch: watching " << kCanaryPrefix
              << " from revision=" << (m_canaryWatchRevision + 1));
}

// ============================================================
// 组装路由快照并推送（调用方须持有 m_registryMutex）
// 新增：按 canary_weights 将 version 权重展开为 ip:port 权重
// ============================================================

void EtcdGrpcConnector::AssembleAndPushRouteUpdated()
{
    NodeNotice notice;

    // ── 收集 nodeType 的 canary 权重 map ──
    // 绕过 CJsonObject/BSON 的解析 bug，直接用简单 JSON 解析
    std::map<std::string, std::map<std::string, int32_t>> typeVerWeights;
    {
        std::lock_guard<std::mutex> lk(m_canaryMutex);
        for (const auto& kv : m_canaryWeights)
        {
            const std::string& rawJson = kv.second;
            std::map<std::string, int32_t> verW;

            // 简单 JSON 解析: {"key":val, ...} 或 {"key":"val", ...}
            size_t pos = 0;
            while (pos < rawJson.size())
            {
                // skip whitespace/brace/comma/colon
                while (pos < rawJson.size() && (rawJson[pos] == ' ' || rawJson[pos] == '{'
                       || rawJson[pos] == '}' || rawJson[pos] == ',' || rawJson[pos] == ':'))
                    ++pos;
                if (pos >= rawJson.size()) break;

                // read key (quoted string)
                if (rawJson[pos] != '"') break;
                size_t keyStart = ++pos;
                while (pos < rawJson.size() && rawJson[pos] != '"') ++pos;
                if (pos >= rawJson.size()) break;
                std::string key = rawJson.substr(keyStart, pos - keyStart);
                ++pos; // skip closing quote

                // skip colon
                while (pos < rawJson.size() && (rawJson[pos] == ' ' || rawJson[pos] == ':'))
                    ++pos;

                // read value (number or quoted string)
                int32_t w = 0;
                if (pos < rawJson.size() && rawJson[pos] == '"')
                {
                    // string value
                    size_t valStart = ++pos;
                    while (pos < rawJson.size() && rawJson[pos] != '"') ++pos;
                    std::string valStr = rawJson.substr(valStart, pos - valStart);
                    ++pos;
                    try { w = std::stoi(valStr); } catch (...) { w = 0; }
                }
                else
                {
                    // number value
                    size_t valStart = pos;
                    while (pos < rawJson.size() && (rawJson[pos] >= '0' && rawJson[pos] <= '9'))
                        ++pos;
                    std::string valStr = rawJson.substr(valStart, pos - valStart);
                    if (!valStr.empty()) try { w = std::stoi(valStr); } catch (...) { w = 0; }
                }
                if (w > 0) verW[key] = w;
            }
            if (!verW.empty()) {
                typeVerWeights[kv.first] = std::move(verW);
                // DEBUG
                std::string dbg = "CanaryParsed[" + kv.first + "]: ";
                for (auto& vw : typeVerWeights[kv.first])
                    dbg += vw.first + "=" + std::to_string(vw.second) + " ";
                GLOG_INFO(dbg);
            }
        }
    }

    // ── 收集 ip:port 权重表（用于最终填充 NodeNotice） ──
    // nodeType → {ip:port → weight}, {ip:port → version → weight}
    struct IpVerW { std::string ipPort; std::string version; int32_t verWeight; };
    std::map<std::string, std::vector<IpVerW>> typeIpList;

    int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto& kv : m_nodeRegistry)
    {
        const std::string& kvIpPort = kv.first;
        auto c = kvIpPort.rfind(':');
        if (c == std::string::npos) continue;
        util::CJsonObject oVal;
        if (!oVal.Parse(kv.second)) continue;

        int32_t  nid  = 0;
        uint32_t wnum = 0;
        std::string ntype, ver;
        oVal.Get("node_id",      nid);
        oVal.Get("worker_num",   wnum);
        oVal.Get("node_type",    ntype);
        oVal.Get("node_version", ver);
        if (ver.empty()) ver = "v1";

        // 僵尸节点过滤：跳过 registered_at 超过 kZombieMaxAge 的条目
        // 新注册的条目带 registered_at 字段；旧注册（无此字段）不受影响
        int32_t registeredAt = 0;
        oVal.Get("registered_at", registeredAt);
        if (registeredAt > 0 && (nowSec - registeredAt) > kZombieMaxAge)
        {
            GLOG_INFO("AssembleAndPushRouteUpdated: skip zombie " << kvIpPort
                      << " (age=" << (nowSec - registeredAt) << "s)");
            continue;
        }

        auto* nr = notice.add_node_arry_reg();
        nr->set_node_ip(kvIpPort.substr(0, c));
        nr->set_node_port(static_cast<uint32_t>(std::stoul(kvIpPort.substr(c + 1))));
        if (nid  > 0)       nr->set_node_id(static_cast<uint32_t>(nid));
        if (!ntype.empty()) nr->set_node_type(ntype);
        nr->set_worker_num(wnum > 0 ? wnum : 1);
        nr->set_node_version(ver);

        // 汇总 ip:port → version 用于权重展开
        typeIpList[ntype].push_back({kvIpPort, ver, 0});
    }

    // ── 展开权重：按 typeVerWeights[ntype] 的 version 权重，均分给同 version 的所有 ip:port ──
    for (auto& typeEntry : typeIpList)
    {
        const std::string& ntype = typeEntry.first;
        auto& ipList = typeEntry.second;

        auto verWeights = typeVerWeights.find(ntype);
        if (verWeights == typeVerWeights.end()) continue; // 无 canary 配置

        // 统计每个 version 有多少个节点
        std::map<std::string, int32_t> verCount;
        for (auto& ipv : ipList)
            verCount[ipv.version]++;

        // 计算每个 ip:port 的最终权重
        for (auto& ipv : ipList)
        {
            auto verW = verWeights->second.find(ipv.version);
            if (verW != verWeights->second.end() && verCount[ipv.version] > 0)
            {
                ipv.verWeight = verW->second / verCount[ipv.version];
            }
            else
            {
                // 版本匹配失败 → fallback: ip:port 直接匹配
                auto directW = verWeights->second.find(ipv.ipPort);
                if (directW != verWeights->second.end() && directW->second > 0)
                    ipv.verWeight = directW->second;
                else
                    ipv.verWeight = 0;
            }
        }

        // 填入 NodeNotice.canary_weights（ip:port → weight）
        // DEBUG: 打印展开后的权重
        {
            std::string dbg = "CanaryExpand[" + ntype + "]: ";
            for (auto& ipv : ipList) {
                if (ipv.verWeight > 0) {
                    dbg += ipv.ipPort + "=" + std::to_string(ipv.verWeight) + " ";
                }
            }
            GLOG_INFO(dbg);
        }

        for (auto& ipv : ipList)
        {
            if (ipv.verWeight > 0)
            {
                (*notice.mutable_canary_weights())[ipv.ipPort] = ipv.verWeight;
            }
        }

        // DEBUG: 确认 protobuf 写入后
        {
            std::string dbg2 = "CanaryNotice[" + ntype + "]: ";
            for (const auto& e : notice.canary_weights())
                dbg2 += e.first + "=" + std::to_string(e.second) + " ";
            GLOG_INFO(dbg2);
        }
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
                                                    uint32_t workerNum, const std::string& nodeVersion)
{
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string val = "{\"node_id\":"     + std::to_string(nodeId)    +
                      ",\"node_type\":\"" + nodeType                  + "\"" +
                      ",\"node_ip\":\""   + ip                        + "\"" +
                      ",\"node_port\":"   + std::to_string(port)      +
                      ",\"worker_num\":"  + std::to_string(workerNum > 0 ? workerNum : 1);
    if (!nodeVersion.empty())
        val += ",\"node_version\":\"" + nodeVersion + "\"";
    val += ",\"registered_at\":" + std::to_string(now);
    val += "}";
    return val;
}

std::string EtcdGrpcConnector::SlotKey(int slot)
{
    return "/thunder/slot/" + std::to_string(slot);
}

} /* namespace net */
