/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdMgrServerConfig.hpp"
#include "labor/Manager.hpp"

namespace net
{

bool CmdMgrServerConfig::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    OrdinaryResponse oRes;
	oRes.set_err_no(0);
	oRes.set_err_msg("OK");
	GetLabor()->SendTo(stMsgShell, oInMsgHead.cmd() + 1, oInMsgHead.seq(),oRes.SerializeAsString());
	((net::Manager*)GetLabor())->SendToWorkerWithMod(0,oInMsgHead, oInMsgBody);//只要发给第一个worker
	return(true);
}

} /* namespace net */
