/*******************************************************************************
* * Project:  Net
 * @file     CmdReloadCustom.hpp
 * @brief
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SYS_CmdReloadCustom_HPP_
#define SRC_CMD_SYS_CmdReloadCustom_HPP_

#include "cmd/Cmd.hpp"

namespace net
{
/**
 * @brief   重新加载custom配置指令
 */
class CmdReloadCustom : public Cmd
{
public:
	CmdReloadCustom() = default;
    virtual ~CmdReloadCustom() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
