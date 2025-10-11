/*******************************************************************************
 * Project:  Net
 * @file     StepIoTimeout.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年10月31日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepIoTimeout.hpp"

namespace net
{

StepIoTimeout::StepIoTimeout(const tagMsgShell& stMsgShell, struct ev_timer* pWatcher)
    : m_stMsgShell(stMsgShell), watcher(pWatcher)
{
}

E_CMD_STATUS StepIoTimeout::Emit(int iErrno,const std::string& strErrMsg,const std::string& strErrShow)
{
	return SendHeartBeat();
}

E_CMD_STATUS StepIoTimeout::Callback(const tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,void* data)
{
	LOG4_TRACE("%s() stMsgShell(%d,%u) ClientAddr(%s) GetConnectIdentify(%s)",__FUNCTION__,
					m_stMsgShell.iFd,m_stMsgShell.ulSeq,GetLabor()->GetClientAddr(m_stMsgShell).c_str(),
					GetLabor()->GetConnectIdentify(m_stMsgShell).c_str());
	GetLabor()->SetHeartBeat(m_stMsgShell);
    return(STATUS_CMD_RUNNING);
}

E_CMD_STATUS StepIoTimeout::SendHeartBeat()
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgHead.set_cmd(CMD_REQ_BEAT);
	oOutMsgHead.set_seq(GetSequence());
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	LOG4_TRACE("%s() stMsgShell(%d,%u) ClientAddr(%s) GetConnectIdentify(%s)",__FUNCTION__,
					m_stMsgShell.iFd,m_stMsgShell.ulSeq,GetLabor()->GetClientAddr(m_stMsgShell).c_str(),
					GetLabor()->GetConnectIdentify(m_stMsgShell).c_str());
	if (GetLabor()->SendTo(m_stMsgShell, oOutMsgHead, oOutMsgBody))
	{
		return(STATUS_CMD_RUNNING);
	}
	else        // SendTo错误会触发断开连接和回收资源
	{
		LOG4_WARN("%s()",__FUNCTION__);
		return(STATUS_CMD_FAULT);
	}
}

E_CMD_STATUS StepIoTimeout::Timeout()
{
	LOG4_TRACE("%s() stMsgShell(%d,%u) ClientAddr(%s) GetConnectIdentify(%s)",__FUNCTION__,
					m_stMsgShell.iFd,m_stMsgShell.ulSeq,GetLabor()->GetClientAddr(m_stMsgShell).c_str(),
					GetLabor()->GetConnectIdentify(m_stMsgShell).c_str());
	GetLabor()->CheckHeartBeat(m_stMsgShell);
    return(STATUS_CMD_FAULT);
}

} /* namespace net */
