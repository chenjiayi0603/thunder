/*******************************************************************************
* * Project:  Net
 * @file     CmdBeat.hpp
 * @brief    心跳包响应
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SYS_CMD_CMDBEAT_HPP_
#define SRC_CMD_SYS_CMD_CMDBEAT_HPP_

#include "cmd/Cmd.hpp"

namespace net
{
/**
 * @brief 服务器心跳指令（工作者）
 */
class CmdBeat : public Cmd
{
public:
    CmdBeat() = default;
    virtual ~CmdBeat() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
