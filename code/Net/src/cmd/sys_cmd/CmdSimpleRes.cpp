/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdSimpleRes.hpp"

namespace net
{

bool CmdSimpleRes::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	LOG4_TRACE("%s() cmd(%u) seq(%u) oInMsgBody(%s)", __FUNCTION__,oInMsgHead.cmd(),oInMsgHead.seq(),oInMsgBody.DebugString().c_str());
    return true;
}

} /* namespace net */
