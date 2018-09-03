/*
 * StepNode.cpp
 *
 *  Created on: 2017年8月1日
 *      Author: chen
 */
#include "StepNode.hpp"

namespace net
{

StepNode::StepNode(const DataMem::MemOperate* pMemOper)
{
    Init();
    if (pMemOper)
    {
        if (pMemOper->has_redis_operate() && (pMemOper->redis_operate().redis_cmd_read().size() == 0)
                        && (pMemOper->redis_operate().redis_cmd_write().size() > 0))
        {//写操作不重发
            m_uiRetrySend = 0;
        }
        else if (pMemOper->has_db_operate() && (DataMem::MemOperate::DbOperate::SELECT != pMemOper->db_operate().query_type()))
        {//写操作不重发
            m_uiRetrySend = 0;
        }
        m_strMsgSerial = pMemOper->SerializeAsString();
    }
}

StepNode::StepNode(const std::string &strBody):m_strMsgSerial(strBody)
{
    Init();
}

StepNode::~StepNode()
{
}


void StepNode::Init()
{
    m_uiTimeOut = 0;
    m_uiTimeOutMax = 3;
    m_uiRetrySend = 1;
    m_storageCallbackSession = NULL;
    m_storageCallbackStep = NULL;
    m_standardCallbackSession = NULL;
    m_standardCallbackStep = NULL;
    m_pSession = NULL;
    m_pUpperStep = NULL;
    m_uiUpperStepSeq = 0;
    m_uiCmd = 0;
    m_uiModFactor = -1;
}

net::E_CMD_STATUS StepNode::Emit(int iErrno , const std::string& strErrMsg , const std::string& strErrShow )
{
	if (m_uiCmd > 0 && m_strNodeType.size() > 0 && m_strMsgSerial.size() > 0)
	{
        MsgHead oOutHead;
        MsgBody oOutBody;
        oOutHead.set_seq(GetSequence());
        oOutHead.set_cmd(m_uiCmd);
        oOutBody.set_body(m_strMsgSerial);
        oOutHead.set_msgbody_len(oOutBody.ByteSize());
        LOG4CPLUS_TRACE_FMT(GetLogger(), "StepNode::Emit m_uiModFactor(%d)!Send cmd[%u] seq[%u] sending",
                        m_uiModFactor,oOutHead.cmd(),oOutHead.seq());
        bool bRet(false);
        if (m_uiModFactor >= 0)
        {
            bRet = Step::SendToWithMod(m_strNodeType,m_uiModFactor,oOutHead,oOutBody);
        }
        else
        {
            bRet = Step::SendToNext(m_strNodeType,oOutHead,oOutBody);
        }
        if (!bRet)
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(), "StepNode Send strNodeType(%s) failed.cmd[%u] seq[%u] send fail",
                            m_strNodeType.c_str(),oOutHead.cmd(),oOutHead.seq());
            return net::STATUS_CMD_FAULT;
        }
        return net::STATUS_CMD_RUNNING;
	}
	else
	{
		LOG4CPLUS_ERROR_FMT(GetLogger(), "error m_uiCmd(%u) or m_strNodeType(%s) or m_strMsgSerial.size(%u).",
		                m_uiCmd,m_strNodeType.c_str(),m_strMsgSerial.size());
		return net::STATUS_CMD_FAULT;
	}
}

net::E_CMD_STATUS StepNode::Timeout()
{
    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s()",__FUNCTION__);
    ++m_uiTimeOut;
    if (m_uiTimeOut < m_uiTimeOutMax)
    {
    	if (m_uiRetrySend > 0)
    	{
    		return Emit();
    	}
        return net::STATUS_CMD_RUNNING;
    }
    if (m_uiModFactor >= 0)//指定节点路由失效的选择另外节点尝试处理
    {
        LOG4CPLUS_INFO_FMT(GetLogger(),"%s() try other method to send, strNodeType(%s) uiTimeOut(%u)",
                        __FUNCTION__,m_strNodeType.c_str(),m_uiTimeOut);
        m_uiModFactor = -1;
        m_uiTimeOut = 0;
        return Emit();
    }
    LOG4CPLUS_ERROR_FMT(GetLogger(),"%s() strNodeType(%s) uiTimeOut(%u)",__FUNCTION__,m_strNodeType.c_str(),m_uiTimeOut);
    return net::STATUS_CMD_COMPLETED;
}

net::E_CMD_STATUS StepNode::Callback(
        const net::tagMsgShell& stMsgShell,
        const MsgHead& oInMsgHead,
        const MsgBody& oInMsgBody,
        void* data)
{
    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s()",__FUNCTION__);
    if(net::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"system response error");
        return net::STATUS_CMD_FAULT;
    }
    if (m_storageCallbackSession)
    {
    	DataMem::MemRsp oRsp;
    	if (!DecodeMemRsp(oRsp,oInMsgBody))
    	{
    		LOG4CPLUS_ERROR_FMT(GetLogger(),"DecodeMemRsp error");
    	}
    	if (GetSession() == NULL)
    	{
    		LOG4CPLUS_ERROR_FMT(GetLogger(), "failed to Get Session(%s,%s)!",
							m_strUpperSessionId.c_str(),m_strUpperSessionClassName.c_str());
			return net::STATUS_CMD_FAULT;
    	}
    	m_storageCallbackSession(oRsp,m_pSession);
    }
    else if (m_storageCallbackStep)
	{
    	if (m_pUpperStep)
    	{
    		DataMem::MemRsp oRsp;
			if (!DecodeMemRsp(oRsp,oInMsgBody))
			{
				LOG4CPLUS_ERROR_FMT(GetLogger(),"DecodeMemRsp error");
			}
    		m_storageCallbackStep(oRsp,m_pUpperStep);
    	}
    	else
    	{
    		LOG4CPLUS_ERROR_FMT(GetLogger(), "m_pStep null");
			return net::STATUS_CMD_FAULT;
    	}
	}
    else if (m_standardCallbackSession)
    {
    	if (GetSession() == NULL)
		{
			LOG4CPLUS_ERROR_FMT(GetLogger(), "failed to Get Session(%s,%s)!",
							m_strUpperSessionId.c_str(),m_strUpperSessionClassName.c_str());
			return net::STATUS_CMD_FAULT;
		}
    	m_standardCallbackSession(oInMsgHead,oInMsgBody,data,m_pSession);
    }
    else if (m_standardCallbackStep)
    {
    	if (m_pUpperStep)
		{
    		m_standardCallbackStep(oInMsgHead,oInMsgBody,data,m_pUpperStep);
		}
		else
		{
			LOG4CPLUS_ERROR_FMT(GetLogger(), "m_pStep null");
			return net::STATUS_CMD_FAULT;
		}
    }
	else
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "m_callbackSession and m_callbackStep null!");
        return net::STATUS_CMD_FAULT;
    }
    if (m_pUpperStep && m_pUpperStep->IsRegistered())
    {
        if (net::STATUS_CMD_RUNNING != m_pUpperStep->Emit())
        {
            LOG4_TRACE("%s() [m_pUpperStep->Emit():%u,%p]",__FUNCTION__,m_pUpperStep->GetSequence(),m_pUpperStep);
            DeleteCallback(m_pUpperStep);
        }
        m_pUpperStep = NULL;
    }
    return net::STATUS_CMD_COMPLETED;
}

bool StepNode::DecodeMemRsp(DataMem::MemRsp &oRsp,const MsgBody& oInMsgBody)
{
	if(!oRsp.ParseFromString(oInMsgBody.body()))
	{
		LOG4CPLUS_ERROR_FMT(GetLogger(), "parse protobuf data fault");
		return false;
	}
	//读存储出错
	if(0 != oRsp.err_no())
	{
		if(oRsp.err_msg().size() > 0)
		{
			LOG4CPLUS_ERROR_FMT(GetLogger(), "StepNode::DecodeMemRsp error %d: %s!",
							oRsp.err_no(),oRsp.err_msg().c_str());
		}
		else
		{
			LOG4CPLUS_ERROR_FMT(GetLogger(), "StepNode::DecodeMemRsp error %d!",oRsp.err_no());
		}
		return false;
	}
	return true;
}


}
