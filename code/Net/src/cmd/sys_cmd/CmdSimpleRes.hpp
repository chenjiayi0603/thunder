/*******************************************************************************
* * Project:  Net
 * @file     CmdSimpleRes.hpp
 * @brief    处理响应消息(不做处理)
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SYS_CMD_CmdSimpleRes_HPP_
#define SRC_CMD_SYS_CMD_CmdSimpleRes_HPP_

#include "cmd/Cmd.hpp"

namespace net
{
/**
 * @brief 处理响应消息(不做处理)
 */
class CmdSimpleRes : public Cmd
{
public:
	CmdSimpleRes() = default;
    virtual ~CmdSimpleRes() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
