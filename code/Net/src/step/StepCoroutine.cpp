#include "step/StepCoroutine.hpp"
#include "NetDefine.hpp"
#include "NetError.hpp"

namespace net
{

StepCoroutine::StepCoroutine() : HttpStep() {}

StepCoroutine::StepCoroutine(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead)
    : HttpStep(stInMsgShell, oInMsgHead)
{
}

StepCoroutine::StepCoroutine(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
    : HttpStep(stInMsgShell, oInMsgHead, oInMsgBody)
{
}

StepCoroutine::StepCoroutine(const tagMsgShell& stInMsgShell, const HttpMsg& oInHttpMsg)
    : HttpStep(stInMsgShell, oInHttpMsg)
{
}

StepCoroutine::~StepCoroutine() = default;

bool StepCoroutine::DoHttpRequest(const HttpAwaitable& awaitable)
{
    if (awaitable.getRequestType() == HttpRequestType::GET)
    {
        return HttpGet(awaitable.getUrl());
    }
    if (awaitable.getHeaders().empty())
    {
        return HttpPost(awaitable.getUrl(), awaitable.getBody());
    }
    return HttpPost(awaitable.getUrl(), awaitable.getBody(), awaitable.getHeaders());
}

E_CMD_STATUS StepCoroutine::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    (void)strErrShow;
    if (iErrno != 0)
    {
        m_iErrno = iErrno;
        m_strErrMsg = strErrMsg;
        OnFail();
        return STATUS_CMD_FAULT;
    }

    if (!m_coStarted)
    {
        m_coStarted = true;
        m_coTask = Run();
        m_coTask.resume();
        if (!m_coTask.done())
        {
            m_coTask.resume();
        }
    }

    if (m_coTask.done())
    {
        if (m_coTask.hasException())
        {
            try
            {
                m_coTask.rethrowIfException();
            }
            catch (...)
            {
                OnFail();
                return STATUS_CMD_FAULT;
            }
        }
        OnSucc();
        return STATUS_CMD_COMPLETED;
    }
    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS StepCoroutine::Callback(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg, void* data)
{
    (void)data;
    if (CMD_RSP_SYS_ERROR == oHttpMsg.status_code())
    {
        LOG4_ERROR("%s() system response error", __FUNCTION__);
        return STATUS_CMD_FAULT;
    }
    m_oResHttpMsg = oHttpMsg;
    m_uiTimeOutCounter = 0;

    if (m_suspendedHandle && !m_suspendedHandle.done())
    {
        m_suspendedHandle.resume();
    }

    if (m_coTask.done())
    {
        if (m_coTask.hasException())
        {
            try
            {
                m_coTask.rethrowIfException();
            }
            catch (...)
            {
                OnFail();
                return STATUS_CMD_FAULT;
            }
        }
        OnSucc();
        return STATUS_CMD_COMPLETED;
    }
    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS StepCoroutine::Callback(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,
                                     void* data)
{
    (void)stMsgShell;
    (void)oInMsgHead;
    (void)oInMsgBody;
    (void)data;
    return STATUS_CMD_COMPLETED;
}

E_CMD_STATUS StepCoroutine::Timeout()
{
    ++m_uiTimeOutCounter;
    if (m_uiTimeOutCounter < m_uiTimeOutMax)
    {
        return STATUS_CMD_RUNNING;
    }
    m_coTask.destroy();
    OnFail();
    return STATUS_CMD_FAULT;
}

} // namespace net
