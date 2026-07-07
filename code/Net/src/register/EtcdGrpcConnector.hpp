/*******************************************************************************
 * Project:  Net
 * @file     EtcdGrpcConnector.hpp
 * @brief    CenterConnector 的 gRPC 实现（Watch 事件驱动 + keepalive unary）
 * @author   cjy
 * @date:    2026年6月16日
 * @note
 *   线程模型:
 *     libev 主线程                   gRPC 专属线程（GrpcThreadMain）
 *     ─────────────                  ──────────────────────────────────────
 *     Init() → 启动线程   →→→      LeaseGrant（unary）
 *     ReportNodeStatus    →→→      SlotTxn（unary） → snapshot → DoStartWatch
 *                         ←←←      ev_async → Registered / RouteUpdated / ConfigUpdated
 *     PutConfig           →→→      client.put（unary）
 *     Destroy()           →→→      PostCmd(Stop) → Cancel Watcher → revoke lease
 *
 *                                   Watcher 内部线程（etcd::Watcher::task_）
 *                                   ─────────────────────────────────────────
 *                                   Watch stream → OnWatchEvent → RouteUpdated
 *                                   Watch ended  → OnWatchEnded → m_watchEnded=true
 *
 *   周期任务（gRPC 线程，每 1s tick）:
 *     每 kKeepAliveRefresh 秒: leasetimetolive + 必要时 re-grant（unary）
 *     每 kPollInterval 秒:     DoPollConfig（unary get，Watch 只覆盖 registry）
 *     m_watchEnded==true:      DoInitialSnapshot + DoStartWatch（重建 Watch）
 *
 *   线程安全:
 *     - etcd::SyncClient 只在 gRPC 线程使用
 *     - m_nodeRegistry 由 m_registryMutex 保护（GrpcThread 写快照，Watcher 线程写事件）
 *     - m_eventQueue 由 m_eventMutex 保护
 *     - m_cmdQueue 由 m_cmdMutex + m_cmdCv 保护
 *     - ev_async_send 是线程安全的（libev 保证）
 ******************************************************************************/
#ifndef ETCD_GRPC_CONNECTOR_HPP_
#define ETCD_GRPC_CONNECTOR_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <ev.h>

#include "labor/CenterConnector.hpp"
#include "util/json/CJsonObject.hpp"

#include <etcd/Response.hpp>

namespace etcd { class SyncClient; class Watcher; }

namespace net
{

class EtcdGrpcConnector : public CenterConnector
{
public:
    explicit EtcdGrpcConnector(const util::CJsonObject& conf);
    ~EtcdGrpcConnector() override;

    EtcdGrpcConnector(const EtcdGrpcConnector&)            = delete;
    EtcdGrpcConnector& operator=(const EtcdGrpcConnector&) = delete;
    EtcdGrpcConnector(EtcdGrpcConnector&&)                 = delete;
    EtcdGrpcConnector& operator=(EtcdGrpcConnector&&)      = delete;

    // ---- 生命周期 ----

    bool Init(struct ev_loop* loop, CenterEventCallback cb, void* user_data) override;
    void Destroy() override;
    const char* Name() const override { return "etcd-grpc"; }

    // ---- 节点注册 & 心跳 ----

    bool ReportNodeStatus(const std::string& node_report, bool is_register) override;

    // ---- 状态查询 ----

    bool   IsConnected() const override  { return m_registered.load(); }
    size_t CenterCount() const override  { return 0; }

    // ---- 连接管理（etcd-grpc 无 TCP 连接，全部空操作） ----

    bool TryConsumeMessage(int, uint32_t, const std::string&,
                           uint32_t, uint32_t, const std::string&) override { return false; }
    bool IsCenterConnection(const std::string&) const override              { return false; }
    void OnConnectionDestroy(const std::string&, int, uint32_t) override    {}

    // ---- 配置下发（B3: PUT /thunder/config/...) ----

    void PutConfig(const std::string& key, const std::string& value) override;

private:
    // ---- 命令队列（libev 线程 → gRPC 线程） ----

    enum class CmdType : uint8_t { Register, PutConfig, Stop };

    struct Cmd
    {
        CmdType     type;
        std::string nodeIp;
        uint32_t    nodePort{};
        std::string nodeType;
        uint32_t    workerNum{};
        std::string configKey;    // for PutConfig
        std::string configValue;  // for PutConfig
    };

    void PostCmd(Cmd cmd);

    // ---- gRPC 线程 ----

    void GrpcThreadMain();
    bool DoRegisterGrpc(etcd::SyncClient& client,
                        const std::string& ip, uint32_t port,
                        const std::string& nodeType, uint32_t workerNum);
    void DoInitialSnapshot(etcd::SyncClient& client);
    void DoStartWatch(etcd::SyncClient& client);
    void DoKeepalive(etcd::SyncClient& client);
    void DoPollConfig(etcd::SyncClient& client);

    // ---- Watcher 回调（Watcher 内部线程） ----

    void OnWatchEvent(etcd::Response resp);
    void OnWatchEnded(bool cancelled);

    // ---- 路由组装（调用方须持有 m_registryMutex 或确保无并发） ----

    void AssembleAndPushRouteUpdated();

    // ---- 事件推送（任意线程 → libev 线程） ----

    void PushEvent(CenterEvent ev);

    // ---- ev_async 回调（libev 线程） ----

    static void AsyncCb(struct ev_loop* loop, struct ev_async* w, int revents);
    void        OnAsync();

    // ---- 工具函数 ----

    static std::string BuildRegistryKey(const std::string& nodeType,
                                        const std::string& ip, uint32_t port);
    static std::string BuildRegistryValue(uint32_t nodeId, const std::string& nodeType,
                                          const std::string& ip, uint32_t port, uint32_t workerNum);
    static std::string SlotKey(int slot);

    // ---- 配置 ----

    util::CJsonObject m_oConf;
    std::string       m_endpoint;   ///< 取第一个 etcd 端点

    // ---- libev 状态（主线程读写） ----

    struct ev_loop*     m_loop{};
    CenterEventCallback m_callback;
    void*               m_userData{};
    ev_async            m_async{};
    bool                m_asyncStarted{false};

    // ---- 事件队列（任意线程写，libev 线程读） ----

    std::mutex              m_eventMutex;
    std::queue<CenterEvent> m_eventQueue;

    // ---- 命令队列（libev 线程写，gRPC 线程读） ----

    std::mutex              m_cmdMutex;
    std::condition_variable m_cmdCv;
    std::queue<Cmd>         m_cmdQueue;

    // ---- gRPC 线程 ----

    std::thread       m_grpcThread;
    std::atomic<bool> m_stopFlag{false};

    // ---- 注册状态 ----

    std::atomic<bool> m_registered{false};

    // ---- gRPC 线程私有（无需锁，仅 gRPC 线程访问） ----

    int64_t     m_leaseId{0};
    uint32_t    m_nodeId{0};
    std::string m_myNodeType;
    std::string m_myNodeIp;
    uint32_t    m_myNodePort{0};
    uint32_t    m_myWorkerNum{0};

    // ---- 路由表（m_registryMutex 保护，GrpcThread 和 Watcher 线程均访问） ----

    std::mutex                          m_registryMutex;
    std::map<std::string, std::string>  m_nodeRegistry;  ///< ip:port → JSON value

    // ---- Watch 状态（GrpcThread 读写；m_watchEnded 由 Watcher 线程写） ----

    std::unique_ptr<etcd::Watcher> m_watcher;
    std::atomic<bool>              m_watchEnded{false};
    int64_t                        m_watchRevision{0};  ///< ls 快照时拿到的 revision

    // ---- 配置轮询状态（仅 gRPC 线程） ----

    std::string m_lastConfigValue;
};

} /* namespace net */

#endif /* ETCD_GRPC_CONNECTOR_HPP_ */
