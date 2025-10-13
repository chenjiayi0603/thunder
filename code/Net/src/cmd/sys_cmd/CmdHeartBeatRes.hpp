/*******************************************************************************
* * Project:  Net
 * @file     CmdHeartBeatRes.hpp
 * @brief    处理HeartBeat响应消息
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SYS_CMD_CmdHeartBeatRes_HPP_
#define SRC_CMD_SYS_CMD_CmdHeartBeatRes_HPP_

#include "cmd/Cmd.hpp"

namespace net
{
/**
 * @brief 处理HeartBeat响应消息
 */
class CmdHeartBeatRes : public Cmd
{
public:
	CmdHeartBeatRes() = default;
    virtual ~CmdHeartBeatRes() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
