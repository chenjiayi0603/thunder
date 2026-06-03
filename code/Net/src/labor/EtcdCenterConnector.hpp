/*******************************************************************************
 * Project:  Net
 * @file     EtcdCenterConnector.hpp
 * @brief    CenterConnector 的 etcd 实现（Phase 0 骨架，全部空实现）
 * @author   cjy
 * @date:    2026年6月3日
 * @note
 *   Phase 0：仅提供符合 CenterConnector 接口的骨架，所有方法均为空实现或
 *   最小返回值，不含任何真实的 etcd 通信逻辑。
 *
 *   Phase 1 开始填充：
 *     - Init()             → 解析 etcd endpoints 配置，创建 etcd 客户端
 *     - ReportNodeStatus() → 写 etcd 租约键，完成节点注册与心跳
 *     - Destroy()          → 撤销租约，关闭客户端
 *
 *   配置项（center.connector = "etcd" 时生效）：
 *     center.etcd_endpoints  — etcd 集群地址，如 "http://127.0.0.1:2379"
 *     center.etcd_dial_timeout_ms  — 连接超时（毫秒，默认 3000）
 *     center.etcd_lease_ttl_sec    — 租约 TTL（秒，默认 30）
 ******************************************************************************/
#ifndef ETCD_CENTER_CONNECTOR_HPP_
#define ETCD_CENTER_CONNECTOR_HPP_

#include <string>
#include "labor/CenterConnector.hpp"
#include "util/json/CJsonObject.hpp"

namespace net
{

/**
 * @brief CenterConnector 的 etcd 后端（Phase 0 骨架）
 *
 * 所有接口方法均为空实现，编译通过即达到 Phase 0 目标。
 * Phase 1 起逐步填充真实的 etcd 注册、心跳、watch 逻辑。
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
     * @brief 初始化（Phase 0 空实现，始终返回 true）
     * @note  Phase 1 将在此处解析 etcd_endpoints 并创建 etcd 客户端
     */
    bool Init(struct ev_loop* loop, CenterEventCallback cb, void* user_data) override;

    /**
     * @brief 销毁（Phase 0 空实现）
     * @note  Phase 1 将在此处撤销租约、关闭 etcd 客户端
     */
    void Destroy() override;

    /** @brief 插件名称标识 */
    const char* Name() const override { return "etcd"; }

    // ---- 节点注册 & 心跳 ----

    /**
     * @brief 向 etcd 写注册键或更新心跳（Phase 0 空实现，始终返回 false）
     * @note  Phase 1 将调用 etcd PUT with lease 实现
     */
    bool ReportNodeStatus(const std::string& node_report, bool is_register) override;

    // ---- Center 连接状态查询 ----

    /**
     * @brief 是否已连接（Phase 0 始终返回 false）
     * @note  etcd 后端无持久 TCP 连接概念，Phase 1 返回客户端就绪状态
     */
    bool IsConnected() const override { return false; }

    /**
     * @brief Center 数量（Phase 0 始终返回 0）
     * @note  etcd 后端不维护 Center 节点计数，此方法语义不适用
     */
    size_t CenterCount() const override { return 0; }

    // ---- 连接管理（etcd 后端无 TCP 连接，全部返回 false / 空操作） ----

    /**
     * @brief 尝试消费 TCP 消息（etcd 不使用 TCP 消息，始终返回 false）
     */
    bool TryConsumeMessage(int iFd, uint32_t ulSeq, const std::string& strIdentify,
                           uint32_t cmd, uint32_t seq, const std::string& body) override;

    /**
     * @brief 判断连接是否属于本插件（etcd 无 TCP 连接，始终返回 false）
     */
    bool IsCenterConnection(const std::string& strIdentify) const override;

    /**
     * @brief 通知连接销毁（etcd 无 TCP 连接，空操作）
     */
    void OnConnectionDestroy(const std::string& strIdentify, int iFd, uint32_t ulSeq) override;

private:
    // ---- Phase 0 保留，Phase 1 填充 ----
    util::CJsonObject m_oConf;  ///< "center" 节点配置（保存供 Phase 1 使用）
};

} /* namespace net */

#endif /* ETCD_CENTER_CONNECTOR_HPP_ */
