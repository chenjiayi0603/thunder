/*******************************************************************************
 * Project:  LogQueue
 * @file     CmdQueueLog.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年6月28日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdQueueLog.hpp"

#ifdef __cplusplus
extern "C" {
#endif
net::Cmd* create()
{
    net::Cmd* pCmd = new analysis::CmdQueueLog();
    return(pCmd);
}
#ifdef __cplusplus
}
#endif

namespace analysis
{

CmdQueueLog::CmdQueueLog():m_boInit(false),m_pLogQueueSession(NULL)
{
}

CmdQueueLog::~CmdQueueLog()
{
}

bool CmdQueueLog::Init()
{
    if (m_boInit)
    {
        return true;
    }
    m_pLogQueueSession = GetLogQueueSession(GetLabor(),GetConfigPath(),GetWorkerIdentify());
    if(!m_pLogQueueSession)
    {
        LOG4_ERROR("failed to get GetLogQueueSession");
        return false;
    }
    m_boInit = true;
    return true;
}

bool CmdQueueLog::AnyMessage(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody)
{
    LOG4CPLUS_DEBUG_FMT(GetLogger(), "oInMsgBody:%s", oInMsgBody.DebugString().c_str());
    behaviour::behaviour oInAsk;
    if (!oInAsk.ParseFromString(oInMsgBody.body()))
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "error %d: login::user_login ParseFromString error!", ERR_INVALID_PROTOCOL);
        Response(ERR_INVALID_PARAMS,stMsgShell,oInMsgHead,oInMsgBody);
        return(false);
    }
    int nErrCode(0);
    if (!m_pLogQueueSession->AppendLog(oInAsk,nErrCode))
    {
        LOG4_WARN("%s() failed to AppendLog nErrCode(%d)",__FUNCTION__,nErrCode);
    }
    return Response(nErrCode,stMsgShell,oInMsgHead,oInMsgBody);
}

bool CmdQueueLog::Response(int iErrno,const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    LOG4_DEBUG("%s() CmdQueueLog::Response iErrno(%d)",__FUNCTION__,iErrno);
    MsgHead oOutMsgHead;
    behaviour::behaviour_ack oOutAsk;
    oOutAsk.set_code(iErrno);
    oOutAsk.set_msg(server_err_msg(iErrno));
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(oInMsgHead.seq());
    if (!SendToClient(stMsgShell, oOutMsgHead, oOutAsk))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        stMsgShell.iFd, stMsgShell.ulSeq);
        return false;
    }
    return true;
}


} /* namespace analysis */
