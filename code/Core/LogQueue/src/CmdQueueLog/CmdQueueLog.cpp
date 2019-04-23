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

MUDULE_CREATE(core::CmdQueueLog);

namespace core
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
    m_pLogQueueSession = GetLogQueueSession();
    if(!m_pLogQueueSession)
    {
        LOG4_ERROR("failed to get GetLogQueueSession");
        return false;
    }
    m_boInit = true;
    return true;
}

bool CmdQueueLog::AnyMessage(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody)
{
    LOG4_DEBUG("oInMsgBody:%s", oInMsgBody.DebugString().c_str());
    logqueue::log oInAsk;
    if (!oInAsk.ParseFromString(oInMsgBody.body()))
    {
        LOG4_ERROR("error %d: logqueue::log ParseFromString error!", ERR_INVALID_PROTOCOL);
        Response(ERR_INVALID_PARAMS,stMsgShell,oInMsgHead);
        return(false);
    }
    int nErrCode = m_pLogQueueSession->AppendLog(oInAsk);
    if (nErrCode)
    {
        LOG4_WARN("%s() failed to AppendLog nErrCode(%d)",__FUNCTION__,nErrCode);
    }
    return Response(nErrCode,stMsgShell,oInMsgHead);
}

bool CmdQueueLog::Response(int iErrno,const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead)
{
    LOG4_DEBUG("%s() CmdQueueLog::Response iErrno(%d)",__FUNCTION__,iErrno);
    logqueue::log_ack oOutAsk;
    oOutAsk.set_code(server_err_code(iErrno));
    oOutAsk.set_msg(server_err_msg(iErrno));
    return net::SendToClient(stMsgShell,oInMsgHead,oOutAsk);
}


} /* namespace core */
