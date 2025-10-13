/*******************************************************************************
 * Project:  Net
 * @file     CmdConnectWorker.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年8月6日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdConnectWorker.hpp"

namespace net
{

bool CmdConnectWorker::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    return(true);
}

bool CmdConnectWorker::Start(const tagMsgShell& stMsgShell, int iWorkerIndex)
{
    MsgHead oMsgHead;
    MsgBody oMsgBody;
    ConnectWorker oConnWorker;
    oConnWorker.set_worker_index(iWorkerIndex);
    oMsgBody.set_body(oConnWorker.SerializeAsString());
    oMsgHead.set_cmd(CMD_REQ_CONNECT_TO_WORKER);
    oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
    LOG4_TRACE("send cmd %d.", oMsgHead.cmd());
    for (int i = 0; i < 3; ++i)
    {
        if (GetLabor()->ExecStep(new StepConnectWorker(stMsgShell, oMsgHead, oMsgBody)))
        {
        	break;
        }
    }
    return(false);
}

} /* namespace net */
