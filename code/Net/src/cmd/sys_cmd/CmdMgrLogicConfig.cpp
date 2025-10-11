/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdMgrLogicConfig.hpp"
#include "labor/Manager.hpp"

namespace net
{

bool CmdMgrLogicConfig::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	OrdinaryResponse oRes;
	oRes.set_err_no(0);
	oRes.set_err_msg("OK");
	GetLabor()->SendTo(stMsgShell, oInMsgHead.cmd() + 1, oInMsgHead.seq(),oRes.SerializeAsString());
	((net::Manager*)GetLabor())->SendToWorker(oInMsgHead, oInMsgBody);
    return(true);
}

} /* namespace net */
