/*******************************************************************************
 * Project:  Net
 * @file     CenterConnector.hpp
 * @brief    Manager↔Center 通信插件抽象接口（策略模式）
 * @author   cjy
 * @date:    2026年5月24日
 * @note
 *   类似于 IoBackend 的策略模式，将 Manager 与注册中心(Center)之间的通信
 *   抽象为可替换的插件。内置实现为 TcpCenterConnector（兼容现有基于 TCP +
 *   Protobuf 的协议），可扩展 HTTP、Etcd、Consul 等后端。
 *
 *   角色关系:
 *     Manager (业务) → CenterConnector (策略接口)
 *           → TcpCenterConnector  — 现有 TCP + Protobuf 协议
 *           → (未来) HttpCenterConnector
 *           → (未来) EtcdCenterConnector
 ******************************************************************************/
#ifndef CENTER_CONNECTOR_HPP_
#define CENTER_CONNECTOR_HPP_

#include <cstdint>
#include <functional>
#include <string>

struct ev_loop;

namespace net
{

// ========== 事件类型（CenterConnector → Manager） ==========

enum class CenterEventType : uint8_t
{
    Registered,          ///< 注册成功，携带 node_id；err != 0 表示注册失败
    HeartbeatOk,         ///< 心跳上报成功（可选关注）
    RouteUpdated,        ///< 路由快照更新，携带 serialized NodeNotice
    ConfigUpdated,       ///< 自定义配置更新，携带配置内容
    NodeStop,            ///< Center 要求本节点停机
    NodeRestartWorkers,  ///< Center 要求重启所有 Worker
    ConnectionLost,      ///< 与所有 Center 失联
    ConnectionRestored,  ///< 重连成功（至少连上一个 Center）
};

// ========== 事件携带数据 ==========

struct CenterEvent
{
    CenterEventType type;
    uint32_t        node_id          = 0;     ///< for Registered
    std::string     route_snapshot;           ///< for RouteUpdated (serialized NodeNotice)
    std::string     config_content;           ///< for ConfigUpdated
    int             errcode          = 0;     ///< 错误码（Registered 非 0 表示注册失败）
    std::string     errmsg;
};

// ========== Manager 侧回调 ==========

using CenterEventCallback = std::function<void(const CenterEvent&)>;

// ========== 抽象接口 ==========

/**
 * @brief Manager↔Center 通信插件抽象接口（策略模式）
 * @note
 *   一套 Manager 代码零分支适配多种注册中心后端。
 *   Manager 永远通过 CenterConnector 指针调用，不直接碰 Center 协议。
 */
class CenterConnector
{
public:
    virtual ~CenterConnector() = default;

    // ---- 生命周期 ----

    /**
     * @brief 初始化插件
     * @param loop       libev 事件循环（用于 IO 和定时器）
     * @param cb         事件回调（线程安全要求：当前始终在 libev 线程回调）
     * @param user_data  用户数据（透传给内部上下文）
     * @return 是否初始化成功
     */
    virtual bool Init(struct ev_loop* loop, CenterEventCallback cb, void* user_data) = 0;

    /** @brief 销毁插件，释放所有连接和资源 */
    virtual void Destroy() = 0;

    /** @brief 插件名称（如 "tcp", "http", "etcd", "consul"） */
    virtual const char* Name() const = 0;

    // ---- 节点注册 & 心跳 ----

    /**
     * @brief 向 Center 注册或上报节点状态
     * @param node_report 序列化后的 NodeReport (protobuf)
     * @param is_register true=注册, false=周期性心跳上报
     * @return 是否成功发起（不代表 Center 已确认）
     */
    virtual bool ReportNodeStatus(const std::string& node_report, bool is_register) = 0;

    // ---- Center 连接状态查询 ----

    /** @brief 是否已连接至少一个 Center */
    virtual bool IsConnected() const = 0;

    /** @brief 当前连接的 Center 数量 */
    virtual size_t CenterCount() const = 0;

    // ---- 连接管理（由 Manager 数据接收层调用） ----

    /**
     * @brief 尝试消费来自某条连接的消息（由 Manager::RecvDataAndDispose 调用）
     * @param iFd         文件描述符
     * @param ulSeq       fd 序列号
     * @param strIdentify 连接标识
     * @param cmd         消息命令
     * @param seq         消息序列号
     * @param body        消息体
     * @return true 表示此消息被本插件消费（不再走其他分发路径），
     *         false 表示非本插件管辖的消息
     */
    virtual bool TryConsumeMessage(int iFd, uint32_t ulSeq, const std::string& strIdentify,
                                   uint32_t cmd, uint32_t seq, const std::string& body) = 0;

    /**
     * @brief 判断给定的连接标识是否属于本插件的 Center 连接
     * @param strIdentify 连接标识
     * @return true 表示是 Center 连接
     */
    virtual bool IsCenterConnection(const std::string& strIdentify) const = 0;

    /**
     * @brief 通知插件某条连接即将被销毁（清理内部引用，不 close fd）
     * @param strIdentify 连接标识
     * @param iFd         文件描述符
     * @param ulSeq       fd 序列号
     */
    virtual void OnConnectionDestroy(const std::string& strIdentify, int iFd, uint32_t ulSeq) = 0;

    /**
     * @brief 向 Center 写入配置 (etcd: PUT /thunder/config/...)
     * @param key   配置 key (如 /thunder/config/module/HELLO)
     * @param value 配置 value (JSON)
     */
    virtual void PutConfig(const std::string& key, const std::string& value) { (void)key; (void)value; }
};

} /* namespace net */

#endif /* CENTER_CONNECTOR_HPP_ */
