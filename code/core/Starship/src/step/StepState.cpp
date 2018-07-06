/*
 * StepNodeAccess.cpp
 *
 *  Created on: 2017年8月1日
 *      Author: chen
 */
#include  <limits.h>
#include "StepState.hpp"
#include "OssError.hpp"
#include "OssDefine.hpp"

namespace oss
{
/*
使用例子如：
oss::StepState* pstep = new oss::StepState(stMsgShell,oInHttpMsg);
pstep->AddStateFunc(oss::StateSendToIdentify);//stage 0
pstep->AddStateFunc(oss::StateSendToIdentifyCallback);//stage 1
pstep->SetData(new oss::SendToIdentifyParam(sNodeIdentify,strConfig,oss::CMD_REQ_SERVER_CONFIG));
if (!oss::StepState::Launch(GetLabor(),pstep,3,1))
{
	LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
	return false;
}
 * */
StepState::StepState()
{
	Init();
}

StepState::StepState(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody)
{
	m_stReqMsgShell = stReqMsgShell;
	m_oReqMsgHead = oReqMsgHead;
	m_oReqMsgBody = oReqMsgBody;
	Init();
}

StepState::StepState(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead)
{
	m_stReqMsgShell = stReqMsgShell;
	m_oReqMsgHead = oReqMsgHead;
	Init();
}

StepState::StepState(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg)
{
	m_stReqMsgShell = stReqMsgShell;
	m_oInHttpMsg = oInHttpMsg;
	Init();
}

StepState::~StepState()
{
	if(m_data) {delete m_data;m_data = NULL;}
}

void StepState::Init()
{
	m_uiTimeOutCounter = 0;
	m_uiTimeOutMax = 3;
	m_uiTimeOutRetry = 0;
	m_uiLastState = m_uiState = 0;
	m_uiNextState = -1;
	m_uiStateVecNum = 0;
	m_data = NULL;
	m_iErrno = 0;
	memset(m_StateVec,0,sizeof(m_StateVec));
	m_SuccFunc = NULL;
	m_FailFunc = NULL;
	m_strStepDesc = "StepState";
}

void StepState::AddStateFunc(StateFunc func)
{
	if (m_uiStateVecNum < StepStateVecSize)
	{
		m_StateVec[m_uiStateVecNum++] = func;
	}
}

E_CMD_STATUS StepState::Emit(int iErrno , const std::string& strErrMsg , const std::string& strErrShow )
{
	LOG4_TRACE("%s() uiState(%u) uiStateVecNum(%u) strStepDesc:%s",__FUNCTION__,m_uiState,m_uiStateVecNum,m_strStepDesc.c_str());
	if (0 != iErrno)
	{
		m_iErrno = iErrno;
		m_strErrMsg = strErrMsg;
		OnFail();
		LOG4_WARN("%s() Fail uiLastState(%u) uiState(%u)",__FUNCTION__,m_uiLastState,m_uiState);
		return STATUS_CMD_FAULT;
	}
	if (m_uiNextState >= 0)
    {
        //如果修改了状态则运行该状态(因为可以在回调中修改状态)
        LOG4_TRACE("%s() uiLastState(%u) next uiState(%u)",__FUNCTION__,m_uiLastState,m_uiState);
        m_uiState = m_uiNextState;
        m_uiNextState = -1;
    }
	if (m_uiState < m_uiStateVecNum)
	{
		m_uiLastState = m_uiState;
		LOG4_TRACE("%s() uiLastState(%u) uiState(%u) before run",__FUNCTION__,m_uiLastState,m_uiState);
		if (m_StateVec[m_uiState])
		{
			m_StageClock.Start(m_uiState);
			bool boRet = m_StateVec[m_uiState](this);
			m_StageClock.EndClock();
			LOG4_TRACE("%s() uiLastState(%u) uiState(%u) after run",__FUNCTION__,m_uiLastState,m_uiState);
			if (!boRet)
			{
				OnFail();
				LOG4_TRACE("%s() Fail uiLastState(%u) uiState(%u) uiStateVecNum(%u)",__FUNCTION__,m_uiLastState,m_uiState,m_uiStateVecNum);
				return STATUS_CMD_FAULT;
			}
			if (m_uiNextState >= 0)
            {
                //如果修改了状态则运行该状态
                LOG4_TRACE("%s() uiLastState(%u) next uiState(%u) uiStateVecNum(%u)",__FUNCTION__,m_uiLastState,m_uiState,m_uiStateVecNum);
                m_uiState = m_uiNextState;
                m_uiNextState = -1;
                return STATUS_CMD_RUNNING;
            }
			++m_uiState;//默认转为下一个状态，如果需要另行设置状态则需要自己设置
			if (m_uiState < m_uiStateVecNum)
			{
				LOG4_TRACE("%s() uiLastState(%u) next uiState(%u) uiStateVecNum(%u)",__FUNCTION__,m_uiLastState,m_uiState,m_uiStateVecNum);
				return STATUS_CMD_RUNNING;
			}
		}
	}
	OnSucc();
	LOG4_TRACE("%s() complete uiState(%u) uiLastState(%u) uiStateVecNum(%u)",__FUNCTION__,m_uiState,m_uiLastState,m_uiStateVecNum);
	return STATUS_CMD_COMPLETED;
}

bool StepState::Launch(OssLabor* pLabor,StepState *step,uint32 uiTimeOutMax,uint8 uiToRetry,double dTimeout)
{
	if (step == NULL)
	{
		LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"%s() null step",__FUNCTION__);
		return(false);
	}
	if (!pLabor->RegisterCallback(step,dTimeout))
	{
		LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"%s() RegisterCallback error",__FUNCTION__);
		delete step;
		step = NULL;
		return(false);
	}
	step->SetTimeOutMax(uiTimeOutMax);
	if (uiToRetry)step->SetTimeOutRetry();
	step->InitClock();
	LOG4CPLUS_INFO_FMT(pLabor->GetLogger(),"%s() StepState::Register StepState(%p) uiTimeOutMax(%u) uiToRetry(%u) dTimeout(%lf)",
				__FUNCTION__,step,uiTimeOutMax,uiToRetry,dTimeout);
	int nRet = step->Emit();
	if (STATUS_CMD_RUNNING != nRet)
	{
		pLabor->DeleteCallback(step);
	}
	return (STATUS_CMD_FAULT != nRet);
}

void StepState::AsyncSend(const oss::tagMsgShell& stMsgShell,const std::string& strBody,int iCmd,StateFunc callback)
{
	AddStateFunc(StateSendToMsgShell);//stage 0
	AddStateFunc(callback);//stage 1
	SetData(new oss::SendToMsgShellParam(stMsgShell,strBody,iCmd));
	m_strStepDesc = std::string("StepState:") + __FUNCTION__;
}
void StepState::AsyncSend(const std::string &strToIdentify,const std::string& strBody,int iCmd,StateFunc callback)
{
	AddStateFunc(StateSendToIdentify);//stage 0
	AddStateFunc(callback);//stage 1
	SetData(new oss::SendToIdentifyParam(strToIdentify,strBody,iCmd));
	m_strStepDesc = std::string("StepState:") + strToIdentify + " " + __FUNCTION__;
}
void StepState::AsyncSend(const oss::tagMsgShell& stMsgShell,const MsgHead& oMsgHead, const MsgBody& oMsgBody,StateFunc callback)
{
	AddStateFunc(StateSendPbToMsgShell);//stage 0
	AddStateFunc(callback);//stage 1
	SetData(new oss::SendPbToMsgShellParam(stMsgShell,oMsgHead,oMsgBody));
	m_strStepDesc = std::string("StepState:") + __FUNCTION__;
}
void StepState::AsyncSend(const std::string &strToIdentify,const MsgHead& oMsgHead, const MsgBody& oMsgBody,StateFunc callback)
{
	AddStateFunc(StateSendPbToIdentify);//stage 0
	AddStateFunc(callback);//stage 1
	SetData(new oss::SendPbToIdentifyParam(strToIdentify,oMsgHead,oMsgBody));
	m_strStepDesc = std::string("StepState:") + strToIdentify + " " + __FUNCTION__;
}

bool StepState::Register(OssLabor* pLabor,StepState *step,uint32 uiTimeOutMax,uint8 uiToRetry,double dTimeout)
{
	if (step == NULL)
	{
		LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"%s() null step",__FUNCTION__);
		return(false);
	}
	if (!pLabor->RegisterCallback(step,dTimeout))
	{
		LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"%s() RegisterCallback error",__FUNCTION__);
		delete step;
		step = NULL;
		return(false);
	}
	step->SetTimeOutMax(uiTimeOutMax);
	if (uiToRetry)step->SetTimeOutRetry();
	step->InitClock();
	LOG4CPLUS_INFO_FMT(pLabor->GetLogger(),"%s() StepState::Register StepState(%p) uiTimeOutMax(%u) uiToRetry(%u) dTimeout(%lf)",
			__FUNCTION__,step,uiTimeOutMax,uiToRetry,dTimeout);
	return true;
}

E_CMD_STATUS StepState::Timeout()
{
    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s()",__FUNCTION__);
    ++m_uiTimeOutCounter;
    if (m_uiTimeOutCounter < m_uiTimeOutMax)
    {
    	if (m_uiTimeOutRetry > 0)
    	{
    	    SetNextState(m_uiState - 1);
    		LOG4_WARN("%s() retry last stage. uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u) StepState(%p,%u) Timeout AlarmClock(%d,%d)",
    		    		__FUNCTION__,m_uiTimeOutCounter,m_uiTimeOutMax,m_uiTimeOutRetry,this,m_uiState,m_StageClock.boInit,m_StageClock.boStart);
    		m_StageClock.AlarmClock();
    		return Emit();//retry last stage
    	}
        return STATUS_CMD_RUNNING;
    }
    LOG4_ERROR("%s() uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u) StepState(%p,%u)",
    		__FUNCTION__,m_uiTimeOutCounter,m_uiTimeOutMax,m_uiTimeOutRetry,this,m_uiState);
    OnFail();
    return STATUS_CMD_FAULT;
}

E_CMD_STATUS StepState::Callback(
        const oss::tagMsgShell& stMsgShell,
        const MsgHead& oInMsgHead,
        const MsgBody& oInMsgBody,
        void* data)
{
    LOG4CPLUS_TRACE_FMT(GetLogger(),"%s()",__FUNCTION__);
    if(oss::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"system response error");
        return oss::STATUS_CMD_FAULT;
    }
    m_oResMsgHead = oInMsgHead;
    m_oResMsgBody = oInMsgBody;
    //继续下一个状态
	LOG4_TRACE("%s() continue next uiState(%u) uiLastState(%u) ",__FUNCTION__,m_uiState,m_uiLastState);
	m_uiTimeOutCounter = 0;//新状态重置超时计数
	return Emit();
}

E_CMD_STATUS StepState::Callback(
                        const tagMsgShell& stMsgShell,
                        const HttpMsg& oHttpMsg,
                        void* data)
{
	LOG4CPLUS_TRACE_FMT(GetLogger(),"%s()",__FUNCTION__);
	m_oResHttpMsg = oHttpMsg;
	//继续下一个状态
	LOG4_TRACE("%s() continue next uiState(%u) uiLastState(%u) ",__FUNCTION__,m_uiState,m_uiLastState);
	m_uiTimeOutCounter = 0;//新状态重置超时计数
	return Emit();
}

bool StepState::SendTo(const tagMsgShell& stMsgShell){return Step::SendTo(stMsgShell);}//&& CoroutineYield();是协程函数则放弃执行权
bool StepState::SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	return(Step::SendTo(stMsgShell, oMsgHead, oMsgBody));
}
bool StepState::SendTo(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	return(Step::SendTo(strIdentify, oMsgHead, oMsgBody));
}
bool StepState::SendTo(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg)
{
	return(HttpStep::SendTo(stMsgShell, oHttpMsg));
}
bool StepState::SendToNext(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	return (Step::SendToNext(strNodeType, oMsgHead, oMsgBody));
}
bool StepState::SendToWithMod(const std::string& strNodeType, unsigned int uiModFactor,
		const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	return (Step::SendToWithMod(strNodeType, uiModFactor, oMsgHead, oMsgBody));
}
bool StepState::SendToProxy(const DataMem::MemOperate* pMemOper,std::string strNodeType)
{
	if (pMemOper->has_db_operate() && (DataMem::MemOperate::DbOperate::SELECT != pMemOper->db_operate().query_type()))
	{
		m_uiTimeOutRetry = 0;//访问mysql的写操作不重发
	}
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	oMsgBody.set_body(pMemOper->SerializeAsString());
	oMsgHead.set_cmd(CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	return Step::SendToNext(strNodeType, oMsgHead, oMsgBody);
}
bool StepState::RecvFromProxy(DataMem::MemRsp &oMemRsp,bool &boNeedMore)
{
	if (!oMemRsp.ParseFromString(m_oResMsgBody.body()))
	{
		LOG4_ERROR("%s() StepLoadAiEngineQuestions::Callback,oMemRsp format error!",__FUNCTION__);
		return false;
	}
	if (oMemRsp.err_no())
	{
		if (oMemRsp.err_msg().size() > 0)
		{
			LOG4_ERROR("%s() dataproxy error %d: %s!",__FUNCTION__,
							oMemRsp.err_no(), oMemRsp.err_msg().c_str());
		}
		else
		{
			LOG4_ERROR("%s() dataproxy error %d!",__FUNCTION__,oMemRsp.err_no());
		}
		return false;
	}
	boNeedMore = oMemRsp.curcount() < oMemRsp.totalcount() ? true:false;
	return true;
}

bool StepState::RecvFromProxy(DataMem::MemRsp &oMemRsp)
{
	if (!oMemRsp.ParseFromString(m_oResMsgBody.body()))
	{
		LOG4_ERROR("%s() StepLoadAiEngineQuestions::Callback,oMemRsp format error!",__FUNCTION__);
		return false;
	}
	if (oMemRsp.err_no())
	{
		if (oMemRsp.err_msg().size() > 0)
		{
			LOG4_ERROR("%s() dataproxy error %d: %s!",__FUNCTION__,
							oMemRsp.err_no(), oMemRsp.err_msg().c_str());
		}
		else
		{
			LOG4_ERROR("%s() dataproxy error %d!",__FUNCTION__,oMemRsp.err_no());
		}
		return false;
	}
	return true;
}

bool StepState::SendBack(const std::string &body,int iCode)
{
	if (m_oReqMsgHead.cmd() > 0)
	{
		MsgHead oOutMsgHead = m_oReqMsgHead;
		MsgBody oOutMsgBody;
		oOutMsgBody.set_body(body);
		oOutMsgHead.set_cmd(m_oReqMsgHead.cmd() + 1);
		oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
		if (!GetLabor()->SendTo(m_stReqMsgShell, oOutMsgHead, oOutMsgBody))
		{
			LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", m_stReqMsgShell.iFd, m_stReqMsgShell.ulSeq);
			return false;
		}
	}
	else
	{
		HttpMsg oHttpMsg;
		oHttpMsg.set_type(HTTP_RESPONSE);
		oHttpMsg.set_status_code(iCode);
		oHttpMsg.set_http_major(m_oInHttpMsg.http_major());
		oHttpMsg.set_http_minor(m_oInHttpMsg.http_minor());
		oHttpMsg.set_body(body);
		if (!GetLabor()->SendTo(m_stReqMsgShell, oHttpMsg))
		{
			LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", m_stReqMsgShell.iFd, m_stReqMsgShell.ulSeq);
			return false;
		}
	}
	return true;
}
//参数 string
//tagMsgShell
bool StateSendToMsgShell(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendToMsgShellParam,state,"StateSendToMsgShell(%d,%u)",
			pStageParam->m_stMsgShell.iFd,pStageParam->m_stMsgShell.ulSeq);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	oMsgHead.set_cmd(pStageParam->m_cmd);
	oMsgHead.set_seq(state->GetSequence());
	oMsgBody.set_body(pStageParam->m_strBody);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	return state->SendTo(pStageParam->m_stMsgShell,oMsgHead,oMsgBody);
}

bool StateSendToMsgShellCallback(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendToMsgShellParam,state,"StateSendToMsgShellCallback ok");
	return true;
}
//Identify
bool StateSendToIdentify(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendToIdentifyParam,state,
			"StateSendToIdentify strToIdentify:%s",pStageParam->m_strToIdentify.c_str());
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	oMsgHead.set_cmd(pStageParam->m_cmd);
	oMsgHead.set_seq(state->GetSequence());
	oMsgBody.set_body(pStageParam->m_strBody);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	return state->SendTo(pStageParam->m_strToIdentify,oMsgHead,oMsgBody);
}
bool StateSendToIdentifyCallback(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendToIdentifyParam,state,
			"StateSendToIdentifyCallback strToIdentify:%s",pStageParam->m_strToIdentify.c_str());
	return true;
}
//参数 pb
//tagMsgShell
bool StateSendPbToMsgShell(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendPbToMsgShellParam,state,
			"StateSendToMsgShell(%d,%u)",pStageParam->m_stMsgShell.iFd,pStageParam->m_stMsgShell.ulSeq);
	pStageParam->m_oMsgHead.set_seq(state->GetSequence());//重新设置seq
	return state->SendTo(pStageParam->m_stMsgShell,pStageParam->m_oMsgHead,pStageParam->m_oMsgBody);
}

bool StateSendPbToMsgShellCallback(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendPbToMsgShellParam,state,"StateSendPbToMsgShellCallback ok");
	return true;
}
//Identify
bool StateSendPbToIdentify(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendPbToIdentifyParam,state,
			"StateSendToIdentify strToIdentify:%s",pStageParam->m_strToIdentify.c_str());
	pStageParam->m_oMsgHead.set_seq(state->GetSequence());//重新设置seq
	return state->SendTo(pStageParam->m_strToIdentify,pStageParam->m_oMsgHead,pStageParam->m_oMsgBody);
}
bool StateSendPbToIdentifyCallback(StepState* state)
{
	STAGE_TEST_PARAM_LOG(SendPbToIdentifyParam,state,
			"StateSendToIdentifyCallback strToIdentify:%s",pStageParam->m_strToIdentify.c_str());
	return true;
}

}
