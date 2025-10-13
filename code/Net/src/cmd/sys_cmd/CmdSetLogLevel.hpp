/*******************************************************************************
 * Project:  Net
 * @file     CMD_NODE_CmdSetLogLevel_HPP_.hpp
 * @brief
 * @author   cjy
 * @date:    2019年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef CMD_NODE_CmdSetLogLevel_HPP_
#define CMD_NODE_CmdSetLogLevel_HPP_

#include "protocol/oss_sys.pb.h"
#include "cmd/Cmd.hpp"

namespace net
{
/**
 * @brief   设置日志指令
 * @author  Tommy
 * @date    2019年8月9日
 * @note
 */
class CmdSetLogLevel : public Cmd
{
public:
	CmdSetLogLevel()= default;
    virtual ~CmdSetLogLevel()= default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif /* CMD_NODE_NOTICE_HPP_ */
