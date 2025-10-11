/*******************************************************************************
 * Project:  Net
 * @file     CmdHeartBeatRes.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdHeartBeatRes.hpp"

namespace net
{

bool CmdHeartBeatRes::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	LOG4_TRACE("%s() CMD_RSP_BEAT Identify(%s) stMsgShell(%d,%u) oInMsgHead(%u,%u)",__FUNCTION__,
			GetLabor()->GetConnectIdentify(stMsgShell).c_str(), stMsgShell.iFd,stMsgShell.ulSeq,oInMsgHead.cmd(),oInMsgHead.seq());
	GetLabor()->SetHeartBeat(stMsgShell);
    return true;
}

} /* namespace net */
