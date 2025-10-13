/*******************************************************************************
* * Project:  Net
 * @file     CmdReloadModule.hpp
 * @brief
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMD_SYS_CmdReloadModule_HPP_
#define SRC_CMD_SYS_CmdReloadModule_HPP_

#include "cmd/Cmd.hpp"

namespace net
{
/**
 * @brief   重新加载模块
 */
class CmdReloadModule : public Cmd
{
public:
	CmdReloadModule() = default;
    virtual ~CmdReloadModule() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
