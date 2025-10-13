/*******************************************************************************
 * Project:  Net
 * @file     Step.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年7月27日
 * @note
 * Modify history:
 ******************************************************************************/
#include "Step.hpp"
#include "labor/Worker.hpp"

namespace net
{

Step::Step(Step* pNextStep)
    : m_pNextStep(pNextStep)
{
    AddNextStepSeq(pNextStep);
}

Step::Step(const tagMsgShell& stReqMsgShell,Step* pNextStep)
    : m_stReqMsgShell(stReqMsgShell),
	  m_pNextStep(pNextStep)
{
    AddNextStepSeq(pNextStep);
}

Step::Step(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, Step* pNextStep)
    : m_stReqMsgShell(stReqMsgShell), m_oReqMsgHead(oReqMsgHead),
      m_pNextStep(pNextStep)
{
    AddNextStepSeq(pNextStep);
}

Step::Step(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody, Step* pNextStep)
    : m_stReqMsgShell(stReqMsgShell), m_oReqMsgHead(oReqMsgHead), m_oReqMsgBody(oReqMsgBody)
{
    AddNextStepSeq(pNextStep);
}

Step::~Step()
{
    if (IsRegistered())
    {
        LOG4_TRACE("step %u destruct, m_pNextStep 0x%p", GetSequence(), m_pNextStep);
    }
    SAFE_FREE(m_pTimeoutWatcher);
    if (m_pNextStep)
    {
        if (!m_pNextStep->IsRegistered())
        {
        	SAFE_DELETE(m_pNextStep);
        }
    }
    m_setNextStepSeq.clear();
    m_setPreStepSeq.clear();
    SAFE_DELETE(m_data);
}

bool Step::RegisterCallback(Step* pStep, ev_tstamp dTimeout)
{
    bool bRegisterResult = false;
    bRegisterResult = GetLabor()->RegisterCallback(GetSequence(), pStep, dTimeout);
    if (bRegisterResult && (m_pNextStep == pStep))
    {
        m_setNextStepSeq.insert(pStep->GetSequence());
    }
    return(bRegisterResult);
}

void Step::DelayNextStep()
{
    if (m_pNextStep)
    {
        if (m_pNextStep->IsRegistered())
        {
            m_pNextStep->DelayTimeout();
        }
    }
}

bool Step::NextStep(Step* pNextStep, int iErrno, const std::string& strErrMsg, const std::string& strErrClientShow)
{
    if (pNextStep)
    {
        if (!pNextStep->IsRegistered())
        {
            for (int i = 0; i < 3; ++i)
            {
                if (RegisterCallback(pNextStep))
                {
                    break;
                }
            }
        }
        if (pNextStep->IsRegistered())
        {
            if (net::STATUS_CMD_RUNNING != pNextStep->Emit(iErrno, strErrMsg, strErrClientShow))
            {
                DeleteCallback(pNextStep);
            }
            return(true);
        }
    }
    return(false);
}

bool Step::NextStep(int iErrno, const std::string& strErrMsg, const std::string& strErrClientShow)
{
    for (auto seq_iter:m_setNextStepSeq)
    {
        GetLabor()->ExecStep(GetSequence(),seq_iter, iErrno, strErrMsg, strErrClientShow);
    }
    if (m_setNextStepSeq.size() > 0)
    {
        return(true);
    }
    LOG4_TRACE("m_pNextStep 0x%p", m_pNextStep);
    if (m_pNextStep)
    {
        if (!m_pNextStep->IsRegistered())
        {
        	RegisterCallback(m_pNextStep);
        }
        if (m_pNextStep->IsRegistered())
        {
            if (net::STATUS_CMD_RUNNING != m_pNextStep->Emit(iErrno, strErrMsg, strErrClientShow))
            {
                DeleteCallback(m_pNextStep);
                m_pNextStep = nullptr;
            }
            return(true);
        }
        else
        {
            delete m_pNextStep;
            m_pNextStep = nullptr;
        }
    }
    return(false);
}

void Step::SetNextStepNull()
{
    m_setNextStepSeq.clear();
    m_pNextStep = nullptr;
}

uint32 Step::GetSequence()
{
    if (!m_bRegistered)
    {
        return(0);
    }
    if (0 == m_ulSequence)
    {
		m_ulSequence = GetLabor()->GetSequence();
    }
    return(m_ulSequence);
}

void Step::DelayTimeout()
{
    if (m_bRegistered)
    {
    	LOG4_TRACE("step %u DelayTimeout dActiveTime(%lf) dTimeout(%lf)", GetSequence(),m_dActiveTime,m_dTimeout);
        GetLabor()->ResetTimeout(this, m_pTimeoutWatcher);
    }
    else
    {
        m_dActiveTime += m_dTimeout + 0.5;
    }
}

void Step::AddNextStepSeq(Step* pStep)
{
    if (NULL != pStep && pStep->IsRegistered() && m_bRegistered)
    {
    	LOG4_TRACE("step %u AddNextStepSeq %u",GetSequence(),pStep->GetSequence());
        m_setNextStepSeq.insert(pStep->GetSequence());
    }
}

void Step::AddPreStepSeq(Step* pStep)
{
    if (NULL != pStep && pStep->IsRegistered())
    {
        m_setPreStepSeq.insert(pStep->GetSequence());
    }
}

void Step::RemovePreStepSeq(Step* pStep)
{
	if (NULL != pStep && pStep->IsRegistered())
	{
		m_setPreStepSeq.erase(pStep->GetSequence());
	}
}

} /* namespace net */
