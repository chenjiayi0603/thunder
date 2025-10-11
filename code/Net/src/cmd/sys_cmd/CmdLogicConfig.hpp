/*******************************************************************************
* * Project:  Net
 * @file     CmdBeat.hpp
 * @brief    心跳包响应
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SYS_CmdLogicConfig_HPP_
#define SRC_CMD_SYS_CmdLogicConfig_HPP_

#include "cmd/Cmd.hpp"

namespace net
{

class CmdLogicConfig : public Cmd
{
public:
	CmdLogicConfig() = default;
    virtual ~CmdLogicConfig() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
