/*******************************************************************************
 * Project:  Net
 * @file     EtcdCenterConnector.hpp
 * @brief    CenterConnector 的 etcd 实现（Phase 1：注册 + node_id 分配）
 * @author   cjy
 * @date:    2026年6月3日
 * @note
 *   Phase 1 实现：
 *     - Init()             → 解析 etcd_endpoints，申请租约，启动 KeepAlive 定时器
 *     - ReportNodeStatus() → 解析 NodeReport，执行槽位占位注册或幂等续期
 *     - Destroy()          → 撤销租约，停止定时器
 *     - ev_timer KeepAlive（每 3 秒续租）
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

#include <string>
#include <cstdint>
#include <cstddef>
#include "labor/CenterConnector.hpp"
#include "util/json/CJsonObject.hpp"
#include "util/encrypt/base64.h"

#include <ev.h>

struct ev_loop;

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

    // ---- KeepAlive 定时器 ----

    /**
     * @brief ev_timer 静态回调，转发给 OnKeepAliveTimer()
     */
    static void KeepAliveTimerCallback(struct ev_loop* loop, struct ev_timer* w, int revents);

    /**
     * @brief 每 3 秒执行一次续租，失败则 emit ConnectionLost
     */
    void OnKeepAliveTimer();

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
     * @brief 发射事件到 Manager 回调
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

    ev_timer            m_keepAliveTimer{};     ///< 值成员，无需 new/delete
    bool                m_keepAliveTimerStarted = false;
};

} /* namespace net */

#endif /* ETCD_CENTER_CONNECTOR_HPP_ */
