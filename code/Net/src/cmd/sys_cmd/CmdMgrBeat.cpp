/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdMgrBeat.hpp"

namespace net
{

bool CmdMgrBeat::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    MsgHead oOutMsgHead = oInMsgHead;
    MsgBody oOutMsgBody = oInMsgBody;
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    return GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
}

} /* namespace net */
