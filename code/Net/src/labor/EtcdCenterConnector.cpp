/*******************************************************************************
 * Project:  Net
 * @file     EtcdCenterConnector.cpp
 * @brief    CenterConnector 的 etcd 实现（Phase 0 骨架，全部空实现）
 * @author   cjy
 * @date:    2026年6月3日
 * @note
 *   Phase 0 目标：仅保证编译通过，所有方法均为最小空实现。
 *   真实的 etcd 注册/心跳/watch 逻辑从 Phase 1 开始填充。
 ******************************************************************************/
#include "EtcdCenterConnector.hpp"
#include "util/json/CJsonObject.hpp"
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

// 使用独立 logger category，避免依赖 GetLabor() 上下文
namespace
{
log4cplus::Logger& GetEtcdLogger()
{
    static log4cplus::Logger logger = log4cplus::Logger::getInstance("EtcdCenterConnector");
    return logger;
}
} // namespace

#define ETCD_LOG_WARN(msg)  LOG4CPLUS_WARN(GetEtcdLogger(),  msg)

namespace net
{

EtcdCenterConnector::EtcdCenterConnector(const util::CJsonObject& conf)
    : m_oConf(conf)
{
    // Phase 0：仅保存配置，暂不解析 etcd endpoints
}

EtcdCenterConnector::~EtcdCenterConnector()
{
    // Phase 0：析构时无资源需要释放
}

bool EtcdCenterConnector::Init(struct ev_loop* /*loop*/,
                                CenterEventCallback /*cb*/,
                                void* /*user_data*/)
{
    // Phase 0：空实现，返回 true 保证 Manager 启动流程正常
    ETCD_LOG_WARN("EtcdCenterConnector::Init — not yet implemented (Phase 0 骨架)");
    return true;
}

void EtcdCenterConnector::Destroy()
{
    // Phase 0：空实现，Phase 1 将撤销租约并关闭 etcd 客户端
    ETCD_LOG_WARN("EtcdCenterConnector::Destroy — not yet implemented (Phase 0 骨架)");
}

bool EtcdCenterConnector::ReportNodeStatus(const std::string& /*node_report*/,
                                            bool /*is_register*/)
{
    // Phase 0：空实现，返回 false 表示尚未真正上报
    // Phase 1 将通过 etcd PUT with lease 实现注册与心跳
    ETCD_LOG_WARN("EtcdCenterConnector::ReportNodeStatus — not yet implemented (Phase 0 骨架)");
    return false;
}

bool EtcdCenterConnector::TryConsumeMessage(int /*iFd*/,
                                             uint32_t /*ulSeq*/,
                                             const std::string& /*strIdentify*/,
                                             uint32_t /*cmd*/,
                                             uint32_t /*seq*/,
                                             const std::string& /*body*/)
{
    // etcd 后端不走 TCP 消息通道，始终返回 false（消息不归本插件处理）
    return false;
}

bool EtcdCenterConnector::IsCenterConnection(const std::string& /*strIdentify*/) const
{
    // etcd 后端无 TCP 连接，始终返回 false
    return false;
}

void EtcdCenterConnector::OnConnectionDestroy(const std::string& /*strIdentify*/,
                                               int /*iFd*/,
                                               uint32_t /*ulSeq*/)
{
    // etcd 后端无 TCP 连接，空操作
}

} /* namespace net */
