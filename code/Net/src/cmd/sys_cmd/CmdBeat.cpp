/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdBeat.hpp"

namespace net
{

bool CmdBeat::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	LOG4_TRACE("%s() cmd(%u) seq(%u)", __FUNCTION__,oInMsgHead.cmd(),oInMsgHead.seq());
    MsgHead oOutMsgHead = oInMsgHead;
    MsgBody oOutMsgBody = oInMsgBody;
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    return GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
}

} /* namespace net */
