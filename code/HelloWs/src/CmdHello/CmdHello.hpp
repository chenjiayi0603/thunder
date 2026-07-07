/*******************************************************************************
 * Hello 节点：WebSocket/私有协议等走 Cmd 20001，与 ModuleHello HTTP JSON 业务对齐
 ******************************************************************************/
#ifndef SRC_CMDHELLO_CMDHELLO_HPP_
#define SRC_CMDHELLO_CMDHELLO_HPP_

#include "cmd/Cmd.hpp"

namespace core
{

class CmdHello : public net::Cmd
{
public:
	CmdHello() = default;
	~CmdHello() override = default;
	bool Init() override;
	bool AnyMessage(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
	                const MsgBody& oInMsgBody) override;
};

} // namespace core

#endif
