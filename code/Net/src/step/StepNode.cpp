/*
 * StepNode.cpp
 *
 *  Created on: 2020年1月9日
 *      author   cjy
 */
#include "StepNode.hpp"

namespace net
{

StepNode::StepNode(const DataMem::MemOperate* pMemOper,StepParam* pStepParam)
{
	SetData(pStepParam);
    if (pMemOper)
    {
    	m_uiRetrySend = 0;//发送存储暂时都不重发（重发机制由逻辑层控制,避免存储操作的重发时间过短）
//        if (pMemOper->has_redis_operate() && (pMemOper->redis_operate().redis_cmd_read().size() == 0)
//                        && (pMemOper->redis_operate().redis_cmd_write().size() > 0))
//        {//写操作不重发
//            m_uiRetrySend = 0;
//        }
//        else if (pMemOper->has_db_operate() && (DataMem::MemOperate::DbOperate::SELECT != pMemOper->db_operate().query_type()))
//        {//写操作不重发
//            m_uiRetrySend = 0;
//        }
        m_strMsgSerial = std::move(pMemOper->SerializeAsString());
    }
}

StepNode::StepNode(const std::string &strBody,StepParam* pStepParam):m_strMsgSerial(strBody)
{
	SetData(pStepParam);
}

net::E_CMD_STATUS StepNode::Emit(int iErrno , const std::string& strErrMsg , const std::string& strErrShow )
{
	LOG4_TRACE("m_uiCmd(%u) m_strNodeType(%s) m_strMsgSerial.size(%u).",m_uiCmd,m_strNodeType.c_str(),m_strMsgSerial.size());
	if (m_uiCmd > 0)
	{
        MsgHead oOutHead;
        MsgBody oOutBody;
        oOutHead.set_seq(GetSequence());
        oOutHead.set_cmd(m_uiCmd);
        oOutBody.set_body(m_strMsgSerial);
        oOutHead.set_msgbody_len(oOutBody.ByteSize());
        LOG4_TRACE("%s() m_strModFactor(%s)!Send cmd[%u] seq[%u] sending",__FUNCTION__,m_strModFactor.c_str(),oOutHead.cmd(),oOutHead.seq());
        bool bRet(false);
        if (m_strNodeType.size() != 0)
        {
        	if (m_strNodeType.find(':') != std::string::npos)
			{
				bRet = GetLabor()->SendTo(m_strNodeType,oOutHead,oOutBody);//标识符
			}
			else
			{//节点类型
				if (m_strModFactor.size() > 0)
				{
					oOutBody.set_targetid(m_strModFactor);
					oOutHead.set_msgbody_len(oOutBody.ByteSize());
				}
				bRet = GetLabor()->SendToSession(m_strNodeType,oOutHead,oOutBody);
			}
        }
        else if (m_stMsgShell.iFd != 0)//文件描述符
        {
        	bRet = GetLabor()->SendTo(m_stMsgShell,oOutHead,oOutBody);
        }
        if (!bRet)
        {
            LOG4_ERROR("%s() Send strNodeType(%s) and m_stMsgShell(%d,%u) failed.cmd[%u] seq[%u] send fail",
            		__FUNCTION__,m_strNodeType.c_str(),m_stMsgShell.iFd,m_stMsgShell.ulSeq,oOutHead.cmd(),oOutHead.seq());
            return net::STATUS_CMD_FAULT;
        }
        return net::STATUS_CMD_RUNNING;
	}
	else
	{
		LOG4_ERROR("%s() error m_uiCmd(%u)",__FUNCTION__,m_uiCmd);
		return net::STATUS_CMD_FAULT;
	}
}

net::E_CMD_STATUS StepNode::Timeout()
{
    LOG4_TRACE("%s()",__FUNCTION__);
    ++m_uiTimeOut;
    if (m_uiTimeOut < m_uiTimeOutMax)
    {
    	if (m_uiRetrySend > 0)
    	{
    		return Emit();
    	}
        return net::STATUS_CMD_RUNNING;
    }
    if (m_uiRetrySend > 0)
    {
    	if (m_strModFactor.size() > 0)//指定节点路由失效的选择另外节点尝试处理
		{
			LOG4_INFO("%s() try other method to send, strNodeType(%s) uiTimeOut(%u)",__FUNCTION__,m_strNodeType.c_str(),m_uiTimeOut);
			m_strModFactor.clear();
			m_uiTimeOut = 0;
			return Emit();
		}
    }
    LOG4_ERROR("%s() strNodeType(%s) uiTimeOut(%u) GetTimeout(%lf)",__FUNCTION__,m_strNodeType.c_str(),m_uiTimeOut,GetTimeout());
    return net::STATUS_CMD_COMPLETED;
}

net::E_CMD_STATUS StepNode::Callback(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,void* data)
{
    LOG4_TRACE("%s()",__FUNCTION__);
    if(net::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())
    {
        LOG4_ERROR("system response error");
        return net::STATUS_CMD_FAULT;
    }
    if (m_storageCallbackSession)
    {
    	DataMem::MemRsp oRsp;
    	if (!DecodeMemRsp(oRsp,oInMsgBody))
    	{
    		LOG4_ERROR("DecodeMemRsp error");
    	}
    	if (GetSession() == nullptr)
    	{
    		LOG4_ERROR("failed to Get Session(%s,%s)!",m_strUpperSessionId.c_str(),m_strUpperSessionClassName.c_str());
			return net::STATUS_CMD_FAULT;
    	}
    	m_storageCallbackSession(oRsp,m_pSession,GetData());
    }
    else if (m_storageCallbackStep)
	{
    	Step* pUpperStep = GetLabor()->GetStep(m_uiUpperStepSeq);
    	if (pUpperStep)
    	{
    		DataMem::MemRsp oRsp;
			if (!DecodeMemRsp(oRsp,oInMsgBody))
			{
				LOG4_ERROR("DecodeMemRsp error");
			}
    		m_storageCallbackStep(oRsp,pUpperStep,GetData());
    	}
    	else
    	{
    		LOG4_ERROR("pUpperStep null");
			return net::STATUS_CMD_FAULT;
    	}
	}
    else if (m_standardCallbackSession)
    {
    	if (GetSession() == nullptr)
		{
			LOG4_ERROR("failed to Get Session(%s,%s)!",m_strUpperSessionId.c_str(),m_strUpperSessionClassName.c_str());
			return net::STATUS_CMD_FAULT;
		}
    	m_standardCallbackSession(oInMsgHead,oInMsgBody,GetData(),m_pSession);
    }
    else if (m_standardCallbackStep)
    {
    	Step* pUpperStep = GetLabor()->GetStep(m_uiUpperStepSeq);
    	if (pUpperStep)
		{
    		m_standardCallbackStep(oInMsgHead,oInMsgBody,GetData(),pUpperStep);//StepParam * GetData(){return m_data;}
		}
		else
		{
			LOG4_ERROR("m_pStep null");
			return net::STATUS_CMD_FAULT;
		}
    }
	else
    {
        LOG4_ERROR("m_callbackSession and m_callbackStep null!");
        return net::STATUS_CMD_FAULT;
    }
    if (m_uiUpperStepSeq)
    {
    	GetLabor()->ExecStep(m_uiUpperStepSeq);
    }
    return net::STATUS_CMD_COMPLETED;
}

bool StepNode::DecodeMemRsp(DataMem::MemRsp &oRsp,const MsgBody& oInMsgBody)
{
	if(!oRsp.ParseFromString(oInMsgBody.body()))
	{
		LOG4_ERROR("parse protobuf data fault");
		oRsp.set_err_no(20304);//协议错误
		return false;
	}
	//读存储出错
	if(0 != oRsp.err_no())
	{
		LOG4_ERROR("StepNode::DecodeMemRsp error %d: %s!",oRsp.err_no(),oRsp.err_msg().c_str());
		return false;
	}
	return true;
}
void StepNode::SetCallBack(SessionCallbackMem callback,net::Session* pSession,const std::string &nodeType,const std::string& strModFactor,uint32 uiCmd)
{
	m_storageCallbackSession = callback;
	if (pSession->IsPermanent())m_pSession = pSession;//永久会话可直接使用指针获取
	m_strUpperSessionId = pSession->GetSessionId();
	m_strUpperSessionClassName = pSession->GetSessionClass();
	m_strNodeType = nodeType;
	m_uiCmd = uiCmd;
	m_strModFactor = strModFactor;
}
void StepNode::SetCallBack(StepCallbackMem callback,net::Step* pUpperStep,const std::string &nodeType,const std::string& strModFactor,uint32 uiCmd)
{
	m_storageCallbackStep = callback;
	m_uiUpperStepSeq = pUpperStep->GetSequence();
	m_strNodeType = nodeType;
	m_uiCmd = uiCmd;
	m_strModFactor = strModFactor;
}
void StepNode::SetCallBack(SessionCallback callback,net::Session* pSession,const std::string &nodeType,uint32 uiCmd,const std::string& strModFactor)
{
	m_standardCallbackSession = callback;
	if (pSession->IsPermanent())m_pSession = pSession;//永久会话可直接使用指针获取
	m_strUpperSessionId = pSession->GetSessionId();
	m_strUpperSessionClassName = pSession->GetSessionClass();
	m_strNodeType = nodeType;
	m_uiCmd = uiCmd;
	m_strModFactor = strModFactor;
}
void StepNode::SetCallBack(StepCallback callback,net::Step* pUpperStep,const std::string &nodeType,uint32 uiCmd,const std::string& strModFactor)
{
	m_standardCallbackStep = callback;
	m_uiUpperStepSeq = pUpperStep->GetSequence();
	m_strNodeType = nodeType;
	m_uiCmd = uiCmd;
	m_strModFactor = strModFactor;
}

void StepNode::SetCallBack(SessionCallback callback,net::Session* pSession,const tagMsgShell &stMsgShell,uint32 uiCmd,const std::string& strModFactor)
{
	m_standardCallbackSession = callback;
	if (pSession->IsPermanent())m_pSession = pSession;//永久会话可直接使用指针获取
	m_strUpperSessionId = pSession->GetSessionId();
	m_strUpperSessionClassName = pSession->GetSessionClass();
	m_stMsgShell = stMsgShell;
	m_uiCmd = uiCmd;
	m_strModFactor = strModFactor;
}

void StepNode::SetCallBack(StepCallback callback,net::Step* pUpperStep,const tagMsgShell &stMsgShell,uint32 uiCmd,const std::string& strModFactor)
{
	m_standardCallbackStep = callback;
	m_uiUpperStepSeq = pUpperStep->GetSequence();
	m_stMsgShell = stMsgShell;
	m_uiCmd = uiCmd;
	m_strModFactor = strModFactor;
}

net::Session* StepNode::GetSession()
{
	if (nullptr == m_pSession)
	{
		if (m_strUpperSessionId.size() > 0 && m_strUpperSessionClassName.size() > 0)
		{
			m_pSession = GetLabor()->GetSession(m_strUpperSessionId,m_strUpperSessionClassName);
		}
	}
	return m_pSession;
}


}
