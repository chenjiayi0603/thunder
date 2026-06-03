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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "labor/CenterConnector.hpp"
#include "util/json/CJsonObject.hpp"
#include "util/encrypt/base64.h"

#include <ev.h>

namespace net
{

/**
 * @brief CenterConnector 的 etcd 后端（Phase 1）
 *
 * 通过 etcd HTTP v3 gateway 实现节点注册（租约 + 槽位抢占）与续期。
 * 所有 etcd 请求在 libev 主线程同步执行（curl 阻塞调用，TTL=10s，超时<1s）。
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
     * @brief 向 etcd POST 一个请求，返回响应体；失败返回空字符串
     * @param path  路径，如 "/v3/lease/grant"
     * @param body  JSON 请求体
     */
    std::string EtcdPost(const std::string& path, const std::string& body);

    /**
     * @brief 申请租约（TTL=10s），成功后设置 m_leaseId
     * @return 是否成功
     */
    bool LeaseGrant();

    /**
     * @brief 对 m_leaseId 执行一次续租
     * @return 是否成功
     */
    bool KeepAlive();

    /**
     * @brief 撤销租约
     */
    void LeaseRevoke();

    // ---- 注册核心逻辑 ----

    /**
     * @brief 注册算法入口：幂等查询 → 槽位占位 → emit Registered
     * @param ip       节点 IP
     * @param port     节点端口
     * @param nodeType 节点类型
     */
    void DoRegister(const std::string& ip, uint32_t port, const std::string& nodeType);

    /**
     * @brief 查询 /v3/kv/range，返回已存在的 node_id（0 表示不存在）
     * @param registryKey  etcd key，如 /thunder/registry/ip:port
     * @param outNodeId    成功时写入 node_id
     * @return true 表示 key 存在且解析成功
     */
    bool QueryRegistry(const std::string& registryKey, uint32_t& outNodeId);

    /**
     * @brief 构建单次槽位抢占的 txn JSON 字符串
     * @param slot         槽位序号 [1..255]
     * @param slotKey      /thunder/slot/<slot>
     * @param registryKey  /thunder/registry/ip:port
     * @param ipPort       "ip:port" 字符串（写入 slot value）
     * @return JSON 字符串，供 EtcdPost("/v3/kv/txn") 使用
     */
    std::string BuildSlotTxn(int slot,
                              const std::string& slotKey,
                              const std::string& registryKey,
                              const std::string& ipPort,
                              const std::string& nodeType);

    /**
     * @brief 尝试原子占位单个槽位
     * @param slot         槽位序号
     * @param slotKey      /thunder/slot/<slot>
     * @param registryKey  /thunder/registry/ip:port
     * @param ipPort       "ip:port" 字符串
     * @return true 表示占位成功（txn succeeded）
     */
    bool TryClaimSlot(int slot,
                      const std::string& slotKey,
                      const std::string& registryKey,
                      const std::string& ipPort,
                      const std::string& nodeType);

    // ---- KeepAlive 定时器 ----

    /**
     * @brief ev_timer 静态回调，转发给 OnKeepAliveTimer()
     */
    static void KeepAliveTimerCallback(struct ev_loop* loop, struct ev_timer* w, int revents);

    /**
     * @brief 每 3 秒执行一次续租，失败则 emit ConnectionLost
     */
    void OnKeepAliveTimer();

    // ---- Watch 功能（Phase 2） ----

    /**
     * @brief 启动 watch 线程和 ev_async（Init 中调用）
     */
    void StartWatch();

    /**
     * @brief 停止 watch 线程并清理 ev_async（Destroy 中调用）
     */
    void StopWatch();

    /**
     * @brief watch 线程主函数：长连接 streaming POST /v3/watch
     *        断线后自动重连（m_watchStop 为 false 时循环）
     */
    void WatchThreadFunc();

    /**
     * @brief libcurl streaming write callback（静态），调用 OnWatchChunk()
     */
    static size_t WatchWriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata);

    /**
     * @brief 解析 watch 响应 chunk，提取事件写入队列，并唤醒 libev 线程
     * @param chunk  curl 接收的原始文本块（可能是多行 JSON）
     */
    void OnWatchChunk(const std::string& chunk);

    /**
     * @brief ev_async 静态回调，转发给 OnWatchAsync()
     */
    static void WatchAsyncCallback(struct ev_loop* loop, ev_async* w, int revents);

    /**
     * @brief libev 线程中消费 WatchEvent 队列，组装 NodeNotice，发射 RouteUpdated
     */
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
     * @brief libcurl write callback（用于 EtcdPost 接收响应体）
     */
    static size_t CurlWriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata);

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

    std::string         m_endpoint;        ///< etcd 地址，如 "http://127.0.0.1:2379"
    int64_t             m_leaseId         = 0;
    uint32_t            m_nodeId          = 0;
    std::string         m_nodeIp;
    uint32_t            m_nodePort        = 0;
    std::string         m_nodeType;
    bool                m_registered            = false;
    int                 m_keepAliveFailCount    = 0;   ///< 续租连续失败次数, 用于 etcd 恢复后重注册

    ev_timer            m_keepAliveTimer{};     ///< 值成员，无需 new/delete
    bool                m_keepAliveTimerStarted = false;

    // ---- watch 相关（Phase 2） ----

    /**
     * @brief watch 事件（watch 线程 → libev 线程的跨线程传递单元）
     */
    struct WatchEvent
    {
        std::string type;   ///< "PUT" 或 "DELETE"
        std::string key;    ///< base64 解码后的 etcd key
        std::string value;  ///< base64 解码后的 etcd value（JSON 格式）
    };

    std::thread              m_watchThread;
    std::atomic<bool>        m_watchStop{false};
    ev_async                 m_watchAsync{};
    bool                     m_watchAsyncStarted = false;
    int64_t                  m_lastRevision      = 0;   ///< 断线续看：上次消费到的 mod_revision

    std::mutex               m_watchQueueMutex;
    std::vector<WatchEvent>  m_watchQueue;              ///< watch 线程生产，libev 线程消费
};

} /* namespace net */

#endif /* ETCD_CENTER_CONNECTOR_HPP_ */
