/*******************************************************************************
 * Project:  Net
 * @file     CmdDrainMigrate.hpp
 * @brief    Manager→Worker: 热更新排空通知 (允许迁移空闲 fd)
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef CMD_SYS_CMD_DRAIN_MIGRATE_HPP_
#define CMD_SYS_CMD_DRAIN_MIGRATE_HPP_

#include "cmd/Cmd.hpp"

namespace net
{

/**
 * @brief 热更新排空通知
 * @note  Manager 在新 Worker 就绪后、给旧 Worker 发 SIGTERM 前下发 (CMD_WORKER_DRAIN)。
 *        收到后 Worker 置 m_bDrainMigrate=true, EnterDrainMode 时才迁移空闲 fd。
 *        单纯关停 (k8s 缩容/手动 stop) 无此命令, drain 不迁移 fd, 直接排空关闭。
 */
class CmdDrainMigrate : public Cmd
{
public:
    CmdDrainMigrate() = default;
    virtual ~CmdDrainMigrate() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* CMD_SYS_CMD_DRAIN_MIGRATE_HPP_ */
