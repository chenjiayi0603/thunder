/*******************************************************************************
 * Project:  Net
 * @file     CmdDrainMigrate.cpp
 * @brief    Manager→Worker: 热更新排空通知 (允许迁移空闲 fd)
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdDrainMigrate.hpp"
#include "labor/Worker.hpp"

namespace net
{

bool CmdDrainMigrate::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    ((Worker*)GetLabor())->SetDrainMigrate(true);
    LOG4_INFO("CMD_WORKER_DRAIN: hot-reload drain, idle fd migration enabled");
    return(true);
}

} /* namespace net */
