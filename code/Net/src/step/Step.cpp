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

Step::Step() = default;

Step::Step(const tagMsgShell& stReqMsgShell)
    : m_stReqMsgShell(stReqMsgShell)
{
}

Step::Step(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead)
    : m_stReqMsgShell(stReqMsgShell), m_oReqMsgHead(oReqMsgHead)
{
}

Step::Step(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody)
    : m_stReqMsgShell(stReqMsgShell), m_oReqMsgHead(oReqMsgHead), m_oReqMsgBody(oReqMsgBody)
{
}

Step::~Step()
{
    if (IsRegistered())
    {
        LOG4_TRACE("step %u destruct", GetSequence());
    }
    delete m_pTimeoutWatcher; m_pTimeoutWatcher = nullptr;
    delete m_data; m_data = nullptr;
}

bool Step::RegisterCallback(std::unique_ptr<Step> pStep, ev_tstamp dTimeout)
{
    return GetLabor()->RegisterCallback(std::move(pStep), dTimeout);
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

} /* namespace net */
