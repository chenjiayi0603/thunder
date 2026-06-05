/*******************************************************************************
 * Project:  Net
 * @file     EtcdCenterConnector.hpp
 * @brief    CenterConnector 的 etcd 实现（Phase 2：注册 + node_id 分配 + watch）
 * @author   cjy
 * @date:    2026年6月3日
 * @note
 *   Phase 1 实现：
 *     - Init()             → 解析 etcd_endpoints，申请租约，启动 KeepAlive 定时器
 *     - ReportNodeStatus() → 解析 NodeReport，执行槽位占位注册或幂等续期
 *     - Destroy()          → 撤销租约，停止定时器
 *     - ev_timer KeepAlive（每 3 秒续租）
 *
 *   Phase 2 新增：
 *     - watch 线程        → libcurl streaming POST /v3/watch 监听 /thunder/registry/ 前缀
 *     - WatchEvent 队列   → watch 线程生产，ev_async 通知 libev 线程消费
 *     - OnWatchAsync()    → libev 线程消费队列，组装 NodeNotice，发射 RouteUpdated
 *     - DoRegister()      → registry value 改为 JSON（含 node_type/node_ip/node_port）
 *     - QueryRegistry()   → 同步解析 JSON value
 *
 *   注册算法：
 *     1. 申请租约 (TTL=10s)
 *     2. 查 /thunder/registry/ip:port 是否已存在 → 存在则续期，返回 node_id
 *     3. 不存在则从 hash(ip:port)%255+1 开始扫 /thunder/slot/i，原子 txn 占位
 *
 *   配置项（center.connector = "etcd" 时生效）：
 *     center.etcd_endpoints  — etcd 集群地址，如 "http://127.0.0.1:2379"
 *                              多节点用逗号分隔，Init 时取第一个
 ******************************************************************************/
#ifndef ETCD_CENTER_CONNECTOR_HPP_
#define ETCD_CENTER_CONNECTOR_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <log4cplus/logger.h>
#include "labor/CenterConnector.hpp"
#include "util/json/CJsonObject.hpp"
#include "util/encrypt/base64.h"

#include <ev.h>

namespace net { class EtcdHttpConn; class EtcdWatcher; }

namespace net
{

/**
 * @brief CenterConnector 的 etcd 后端
 *
 * 运行在 Manager 进程,在 Manager libev 主循环上自管一条到 etcd 的异步 HTTP 连接
 * (EtcdHttpConn,框架 HttpCodec 编解码),实现节点注册(租约 + 槽位抢占)与续期。
 * 无独立线程,无 libcurl 出站请求(仅 watch snapshot 遗留少量 curl,Phase C 替换)。
 */
class EtcdCenterConnector : public CenterConnector
{
public:
    /**
     * @param conf  配置对象，对应 node.json 中的 "center" 节点
     */
    explicit EtcdCenterConnector(const util::CJsonObject& conf);
    ~EtcdCenterConnector() override;

    EtcdCenterConnector(const EtcdCenterConnector&)            = delete;
    EtcdCenterConnector& operator=(const EtcdCenterConnector&) = delete;
    EtcdCenterConnector(EtcdCenterConnector&&)                 = delete;
    EtcdCenterConnector& operator=(EtcdCenterConnector&&)      = delete;

    /// 注入本节点 logger(由 Manager 在创建后调用)。否则 etcd 日志打到无 appender 的
    /// 硬编码 category 被丢弃,非 Logic 节点完全看不到 etcd 连接/watch 日志(issus #12)。
    void SetLogger(const log4cplus::Logger& logger) { m_logger = logger; }

    // ---- 生命周期 ----

    /**
     * @brief 初始化：解析 etcd_endpoints，申请租约，启动 KeepAlive 定时器
     */
    bool Init(struct ev_loop* loop, CenterEventCallback cb, void* user_data) override;

    /**
     * @brief 销毁：撤销租约，停止定时器
     */
    void Destroy() override;

    /** @brief 插件名称标识 */
    const char* Name() const override { return "etcd"; }

    // ---- 节点注册 & 心跳 ----

    /**
     * @brief 解析 NodeReport，执行槽位占位注册或幂等续期
     */
    bool ReportNodeStatus(const std::string& node_report, bool is_register) override;

    // ---- Center 连接状态查询 ----

    /**
     * @brief 是否已注册（租约有效 + 已完成槽位占位）
     */
    bool IsConnected() const override { return m_registered; }

    /**
     * @brief etcd 后端不维护 Center 节点计数，返回 0
     */
    size_t CenterCount() const override { return 0; }

    // ---- 连接管理（etcd 后端无 TCP 连接，全部返回 false / 空操作） ----

    bool TryConsumeMessage(int iFd, uint32_t ulSeq, const std::string& strIdentify,
                           uint32_t cmd, uint32_t seq, const std::string& body) override;

    bool IsCenterConnection(const std::string& strIdentify) const override;

    void OnConnectionDestroy(const std::string& strIdentify, int iFd, uint32_t ulSeq) override;

private:
    // ---- etcd HTTP 基础操作 ----

    /**
    /**
     * @brief 撤销租约
     */
    void LeaseRevoke();

    // ---- 异步 etcd 操作原语(Phase B: 走 EtcdHttpConn) ----

    /**
     * @brief 查询 /v3/kv/range，返回已存在的 node_id（0 表示不存在）
     * @param registryKey  etcd key，如 /thunder/registry/ip:port
     * @param outNodeId    成功时写入 node_id
     * @param outLease     成功时写入该 key 绑定的 lease（无租约为 0），用于判断是否需重绑
     * @return true 表示 key 存在且解析成功
     */
    bool QueryRegistry(const std::string& registryKey, uint32_t& outNodeId, int64_t& outLease);

    /**
     * @brief 无条件把 slot/registry 两个键重新 PUT 并绑定到【当前】m_leaseId（issus #19）。
     *        用于重启换租约后，etcd 中残留的注册键还绑在旧（已失效）租约的场景：
     *        旧租约过期会删键导致注册中心永久为空，必须重绑到活租约。
     * @return 是否成功
     */
    bool RebindRegistration(uint32_t nodeId, const std::string& registryKey,
                            const std::string& ipPort, const std::string& nodeType);

    /**
     * @brief 注册表自检对账（issus #19 监控）：查自身注册键是否仍存在且绑在当前租约，
     *        缺失或租约不符则打 ERROR 告警并触发重注册。由 KeepAlive 定时器周期调用。
     */
    void SelfAuditRegistry();

    /**
     * @brief 构建单次槽位抢占的 txn JSON 字符串(纯逻辑,无 I/O)
     */
    std::string BuildSlotTxn(int slot,
                              const std::string& slotKey,
                              const std::string& registryKey,
                              const std::string& ipPort,
                              const std::string& nodeType);

    // ---- 异步 etcd 操作原语(Phase B: 从 curl 同步→异步回调, 走 EtcdHttpConn) ----

    void AsyncLeaseGrant(std::function<void(bool ok, int64_t leaseId)> cb);
    void AsyncKeepAlive(std::function<void(bool ok)> cb);
    void AsyncQueryRegistry(const std::string& registryKey,
                            std::function<void(bool found, uint32_t nodeId, int64_t lease)> cb);
    void AsyncTryClaimSlot(int slot, const std::string& slotKey,
                           const std::string& registryKey,
                           const std::string& ipPort, const std::string& nodeType,
                           std::function<void(bool ok)> cb);
    void AsyncRebindRegistration(uint32_t nodeId, const std::string& registryKey,
                                 const std::string& ipPort, const std::string& nodeType,
                                 std::function<void(bool ok)> cb);

    // ---- 异步注册延续链(替代原 DoRegister 同步流程) ----

    /// 入口:确保有 lease(L1) → 查 registry(L2) → 决策 Fresh/Rebind/Claim(L3)
    void DoRegister(const std::string& ip, uint32_t port, const std::string& nodeType);
    /// L1: 无 lease 则补领
    void OnRegEnsureLease();
    /// L2: 查注册键,决策
    void OnRegQuery();
    /// L3: 槽位扫描
    void OnRegScan();
    /// 完成注册:设置状态 + emit Registered / 失败
    void OnRegDone(bool ok, const std::string& errmsg);

    // ---- KeepAlive 定时器 ----

    /**
     * @brief ev_timer 静态回调，转发给 OnKeepAliveTimer()
     */
    static void KeepAliveTimerCallback(struct ev_loop* loop, struct ev_timer* w, int revents);

    /**
     * @brief 每 3 秒执行一次续租，失败则 emit ConnectionLost
     */
    void OnKeepAliveTimer();

    // ---- Watch 功能（Phase C: EtcdWatcher 替代 curl 线程 + ev_async）----

    /// 启动 EtcdWatcher(异步,无线程) + 初始 snapshot 取 revision
    void StartWatch();
    void StopWatch();

    /// period snapshot: 取当前 revision + 全量 registry, 刷新路由/起点
    void DoWatchSnapshot();
    /// watcher 馈入一行 JSON(事件/created/canceled)
    void ProcessWatchLine(const std::string& line);
    /// 消费事件队列, 组装 NodeNotice, 发射 RouteUpdated(单线,无需 ev_async)
    void OnWatchAsync();

    // ---- 工具函数 ----

    /**
     * @brief base64 编码（etcd gateway 要求 key/value 使用 base64）
     */
    static std::string B64(const std::string& s);

    /**
     * @brief base64 解码
     */
    static std::string B64Dec(const std::string& s);

    /**
     * @brief 发射事件到 Manager 回调（ev 按值传入，支持 std::move）
     */
    void Emit(CenterEventType type, CenterEvent ev = {});

    // ---- 配置（构造时保存） ----
    util::CJsonObject m_oConf;  ///< "center" 节点完整配置

    // ---- 运行时状态 ----
    struct ev_loop*     m_loop            = nullptr;
    CenterEventCallback m_callback;
    void*               m_user_data       = nullptr;

    /// 本节点 logger(由 SetLogger 注入);默认值仅为兜底,正常会被 Manager 覆盖。
    log4cplus::Logger   m_logger          = log4cplus::Logger::getInstance("etcd");

    std::string         m_endpoint;        ///< etcd 地址，如 "http://127.0.0.1:2379"

    /// 异步 HTTP 客户端(Phase B: 替代 curl 短请求)
    std::unique_ptr<EtcdHttpConn> m_http;
    /// watch 长连接(Phase C: 替代 curl 线程 + ev_async)
    std::unique_ptr<EtcdWatcher> m_watcher;

    int64_t             m_leaseId         = 0;
    uint32_t            m_nodeId          = 0;
    std::string         m_nodeIp;
    uint32_t            m_nodePort        = 0;
    std::string         m_nodeType;
    uint32_t            m_workerNum       = 0;   ///< 本节点 worker 数, 注册时写入 registry value 供路由建 identify
    bool                m_registered            = false;

    // 异步注册延续链上下文 (issus #24 Phase B)
    bool   m_regInProgress  = false;    ///< 防止 re-entrancy(定时器/心跳同时触发注册)
    int    m_regSlot        = 0;        ///< 槽位扫描位置(1..255; 0=未开始)
    int    m_regStuckTicks  = 0;        ///< #27 注册卡住超时计数(~3s/tick,>10=复位)

    /// 发现到的全部在线节点 ip:port → registry value(JSON)。
    /// 仅 libev 线程(OnWatchAsync)访问, 无需锁。用于每次变更发"全量"路由快照(issus #9)。
    std::map<std::string, std::string> m_nodeRegistry;
    int                 m_keepAliveFailCount    = 0;   ///< 续租连续失败次数, 用于 etcd 恢复后重注册

    ev_timer            m_keepAliveTimer{};     ///< 值成员，无需 new/delete
    bool                m_keepAliveTimerStarted = false;

    // ---- watch 相关（Phase 2） ----

    struct WatchEvent { std::string type, key, value; };
    std::vector<WatchEvent>  m_watchQueue;          ///< 事件队列(单线,无锁)

    int64_t                  m_lastRevision      = 0;

    // ---- 监控指标（单线程, 无 atomic 需要——watcher/connector 都在 m_loop 上）----
    uint64_t    m_metricKeepAliveOk{0};
    uint64_t    m_metricKeepAliveFail{0};
    uint64_t    m_metricWatchReconnect{0};
    uint64_t    m_metricWatchCompactCancel{0};
    uint64_t    m_metricRegistryRebind{0};
    uint64_t    m_metricSelfAuditFail{0};
    int                      m_auditTick = 0;                ///< 自检节流计数（KeepAlive 定时器内）
};

} /* namespace net */

#endif /* ETCD_CENTER_CONNECTOR_HPP_ */
