/*******************************************************************************
* * Project:  Net
 * @file     CmdMgrLogicConfig.hpp
 * @brief
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SYS_CmdMgrLogicConfig_HPP_
#define SRC_CMD_SYS_CmdMgrLogicConfig_HPP_

#include "cmd/Cmd.hpp"

namespace net
{

class CmdMgrLogicConfig : public Cmd
{
public:
	CmdMgrLogicConfig() = default;
    virtual ~CmdMgrLogicConfig() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
