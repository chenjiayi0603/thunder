#include "coro/StepCo20.hpp"
#include "NetError.hpp"
#include "NetDefine.hpp"
#include "labor/Labor.hpp"
#include <ev.h>

namespace net
{

namespace
{
struct CoSleepPayload
{
    StepCo20* context{};
};
} // namespace

void CoSleepTimerTrampoline(struct ev_loop* /*loop*/, ev_timer* w, int /*revents*/)
{
    auto* p = static_cast<CoSleepPayload*>(w->data);
    StepCo20* context = p->context;
    GetLabor()->DelEvent(w);
    delete w;
    delete p;
    if (context->m_context.handle && !context->m_context.handle.done())
    {
        context->m_context.handle.resume();
    }
}

void CoSleepAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept
{
    pStep->m_context.handle = handle;
    auto* w = new ev_timer();
    auto* p = new CoSleepPayload{pStep};
    w->data = static_cast<void*>(p);
    GetLabor()->AddEvent(delaySec, w, CoSleepTimerTrampoline);
}

LibevIo::IoBoolAwaitable::IoBoolAwaitable(StepCo20* pStep,
                                          std::function<bool()> startFn)
    : pStep_(pStep)
    , startFn_(std::move(startFn))
{
}

void LibevIo::IoBoolAwaitable::await_suspend(std::coroutine_handle<> handle) noexcept
{
    pStep_->m_context.handle = handle;
    StartOrFail();
}

bool LibevIo::IoBoolAwaitable::await_resume() noexcept
{
    if (forceResultReady_)
    {
        return forceResult_;
    }
    const HttpMsg& rsp = pStep_->GetLastRspHttpMsg();
    if (rsp.type() == HTTP_RESPONSE)
    {
        const int code = rsp.status_code();
        return code >= 200 && code < 400;
    }
    return true;
}

void LibevIo::IoBoolAwaitable::StartOrFail() noexcept
{
    const bool sent = startFn_ ? startFn_() : false;

    if (!sent)
    {
        forceResultReady_ = true;
        forceResult_ = false;
        if (pStep_->m_context.handle && !pStep_->m_context.handle.done())
        {
            pStep_->m_context.handle.resume();
        }
    }
}

E_CMD_STATUS StepCo20::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    LOG4_TRACE("%s() strStepDesc:%s", __FUNCTION__, m_strStepDesc.c_str());
    
    if (0 != iErrno)
    {
        m_iErrno = iErrno;
        m_strErrMsg = strErrMsg;
        OnCoroutineError(iErrno, strErrMsg);
        return STATUS_CMD_FAULT;
    }
    
    if (m_bCoroutineRunning)
    {
        LOG4_TRACE("%s() coroutine already running", __FUNCTION__);
        return STATUS_CMD_RUNNING;
    }
    
    if (m_bCoroutineCompleted)
    {
        LOG4_TRACE("%s() coroutine already completed", __FUNCTION__);
        // Do not reset m_oAsyncBootstrap here: the outer coroutine is still suspended at
        // final_suspend (suspend_always) and will be destroyed when DeleteCallback deletes this.
        return STATUS_CMD_COMPLETED;
    }
    
    // 启动协程
    m_bCoroutineRunning = true;

    // 单条 AsyncTask：由 StepAsync() 提供；必须延长生命周期，不可 StepAsync() 临时析构
    m_oAsyncBootstrap.emplace(StepAsync());

    // 若在 emplace 内全程未挂起（例如 co_await SendToInternalByNodeTypeAsync 因无路由同步 co_return），
    // promise.return_void 已 NotifyEmitCoroutineSuccess()；必须返回 COMPLETED，否则 Step 仍挂超时并最终对已关闭的 HTTP fd SendTo。
    if (m_bCoroutineCompleted)
    {
        LOG4_TRACE("%s() coroutine finished synchronously (no suspend)", __FUNCTION__);
        return STATUS_CMD_COMPLETED;
    }

    return STATUS_CMD_RUNNING;
}

void StepCo20::NotifyEmitCoroutineSuccess()
{
    if (m_bCoroutineCompleted)
    {
        return;
    }
    m_bCoroutineCompleted = true;
    m_bCoroutineRunning = false;
    OnCoroutineComplete(true);
    // Do NOT call GetLabor()->ExecStep(this, 0.0) here.  That call would re-enter
    // Emit while this coroutine is still on the real call stack, causing Emit to
    // reset m_oAsyncBootstrap and destroy the running coroutine frame (UB / UAF).
    // Instead, Callback detects m_bCoroutineCompleted after resume() and returns
    // STATUS_CMD_COMPLETED so Worker::Dispose calls DeleteCallback for us.
}

E_CMD_STATUS StepCo20::Callback(const tagMsgShell& stMsgShell,
                                const MsgHead& oInMsgHead,
                                const MsgBody& oInMsgBody,
                                void* data)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    
    if (CMD_RSP_SYS_ERROR == oInMsgHead.cmd())
    {
        LOG4_ERROR("system response error");
        return STATUS_CMD_FAULT;
    }
    
    m_oResMsgHead = oInMsgHead;
    m_oResMsgBody = oInMsgBody;
    
    // 重置超时计数器
    m_uiTimeOutCounter = 0;
    
    // 恢复协程执行
    if (m_context.handle && !m_context.handle.done())
    {
        m_context.handle.resume();
        // resume() drives the full chain via symmetric transfer: inner Task → StepAsync
        // AsyncTask → final_suspend(suspend_always).  By the time resume() returns,
        // m_bCoroutineCompleted is true and the outer frame is suspended (not destroyed).
    }
    
    if (m_bCoroutineCompleted)
    {
        // Signal Worker::Dispose to call DeleteCallback, which destroys this step and
        // safely cleans up m_oAsyncBootstrap while the outer coroutine is suspended.
        return STATUS_CMD_COMPLETED;
    }
    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS StepCo20::Callback(const tagMsgShell& stMsgShell,
                                const HttpMsg& oHttpMsg,
                                void* data)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    
    m_oResHttpMsg = oHttpMsg;
    
    // 重置超时计数器
    m_uiTimeOutCounter = 0;
    
    // 恢复协程执行
    if (m_context.handle && !m_context.handle.done())
    {
        m_context.handle.resume();
    }
    
    if (m_bCoroutineCompleted)
    {
        return STATUS_CMD_COMPLETED;
    }
    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS StepCo20::Timeout()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    
    ++m_uiTimeOutCounter;
    if (m_uiTimeOutCounter < m_uiTimeOutMax)
    {
        if (m_uiTimeOutRetry > 0)
        {
            LOG4_WARN("%s() retry. uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u)",
                      __FUNCTION__, m_uiTimeOutCounter, m_uiTimeOutMax, m_uiTimeOutRetry);
            // 重试：重新启动协程
            m_bCoroutineRunning = false;
            m_bCoroutineCompleted = false;
            return Emit();
        }
        return STATUS_CMD_RUNNING;
    }
    
    LOG4_ERROR("%s() timeout exceeded. uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u)",
               __FUNCTION__, m_uiTimeOutCounter, m_uiTimeOutMax, m_uiTimeOutRetry);
    
    OnCoroutineError(ERR_TIMEOUT, "operation timeout");
    return STATUS_CMD_FAULT;
}

StepCo20::IoBoolAwaitable StepCo20::HttpGetAsync(const std::string& strUrl)
{
    return m_context.HttpGetAsync(strUrl);
}

StepCo20::IoBoolAwaitable StepCo20::HttpPostAsync(const std::string& strUrl, const std::string& strBody)
{
    return m_context.HttpPostAsync(strUrl, strBody);
}

StepCo20::IoBoolAwaitable StepCo20::SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg)
{
    return m_context.SendToAsync(stMsgShell, oHttpMsg);
}

StepCo20::IoBoolAwaitable StepCo20::SendToInternalAsync(const tagMsgShell& stMsgShell, MsgHead oMsgHead, MsgBody oMsgBody)
{
    return m_context.SendToInternalAsync(stMsgShell, std::move(oMsgHead), std::move(oMsgBody));
}

StepCo20::IoBoolAwaitable StepCo20::SendToInternalByIdentifyAsync(const std::string& strIdentify, MsgHead oMsgHead, MsgBody oMsgBody)
{
    return m_context.SendToInternalByIdentifyAsync(strIdentify, std::move(oMsgHead), std::move(oMsgBody));
}

StepCo20::IoBoolAwaitable StepCo20::SendToInternalByNodeTypeAsync(const std::string& strNodeType, MsgHead oMsgHead, MsgBody oMsgBody)
{
    return m_context.SendToInternalByNodeTypeAsync(strNodeType, std::move(oMsgHead), std::move(oMsgBody));
}

LibevIo::IoBoolAwaitable LibevIo::HttpGetAsync(const std::string& strUrl)
{
    return IoBoolAwaitable(pStep_, [step = pStep_, strUrl]()
    {
        return step->HttpGet(strUrl);
    });
}

LibevIo::IoBoolAwaitable LibevIo::HttpPostAsync(const std::string& strUrl, const std::string& strBody)
{
    return IoBoolAwaitable(pStep_, [step = pStep_, strUrl, strBody]()
    {
        return step->HttpPost(strUrl, strBody);
    });
}

LibevIo::IoBoolAwaitable LibevIo::SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg)
{
    return IoBoolAwaitable(pStep_, [step = pStep_, stMsgShell, oHttpMsg]()
    {
        return step->SendTo(stMsgShell, oHttpMsg);
    });
}

LibevIo::IoBoolAwaitable LibevIo::SendToInternalAsync(const tagMsgShell& stMsgShell, MsgHead oMsgHead, MsgBody oMsgBody)
{
    return IoBoolAwaitable(pStep_, [step = pStep_, stMsgShell, oMsgHead = std::move(oMsgHead), oMsgBody = std::move(oMsgBody)]() mutable
    {
        oMsgHead.set_seq(step->GetSequence());
        oMsgHead.set_msgbody_len(static_cast<uint32_t>(oMsgBody.ByteSizeLong()));
        return GetLabor()->SendTo(stMsgShell, oMsgHead, oMsgBody);
    });
}

LibevIo::IoBoolAwaitable LibevIo::SendToInternalByIdentifyAsync(const std::string& strIdentify, MsgHead oMsgHead, MsgBody oMsgBody)
{
    return IoBoolAwaitable(pStep_, [step = pStep_, strIdentify, oMsgHead = std::move(oMsgHead), oMsgBody = std::move(oMsgBody)]() mutable
    {
        oMsgHead.set_seq(step->GetSequence());
        oMsgHead.set_msgbody_len(static_cast<uint32_t>(oMsgBody.ByteSizeLong()));
        return GetLabor()->SendTo(strIdentify, oMsgHead, oMsgBody);
    });
}

LibevIo::IoBoolAwaitable LibevIo::SendToInternalByNodeTypeAsync(const std::string& strNodeType, MsgHead oMsgHead, MsgBody oMsgBody)
{
    return IoBoolAwaitable(pStep_, [step = pStep_, strNodeType, oMsgHead = std::move(oMsgHead), oMsgBody = std::move(oMsgBody)]() mutable
    {
        oMsgHead.set_seq(step->GetSequence());
        oMsgHead.set_msgbody_len(static_cast<uint32_t>(oMsgBody.ByteSizeLong()));
        return GetLabor()->SendToSession(strNodeType, oMsgHead, oMsgBody);
    });
}

void StepCo20::OnCoroutineComplete(bool bSuccess)
{
    LOG4_TRACE("%s() success:%d", __FUNCTION__, bSuccess);
}

void StepCo20::OnCoroutineError(int iErrno, const std::string& strErrMsg)
{
    LOG4_ERROR("%s() errno:%d msg:%s", __FUNCTION__, iErrno, strErrMsg.c_str());
    
    m_iErrno = iErrno;
    m_strErrMsg = strErrMsg;
}

void StepCo20::ResponseToClient(int iCode, const std::string& strBody)
{
    if (m_oReqMsgHead.cmd() == 0) 
    {
        // HTTP 响应
        GetLabor()->SendToClient(m_stReqMsgShell, m_oInHttpMsg, strBody, iCode);
    }
    else
    {
        // 普通消息响应
        GetLabor()->SendToClient(m_stReqMsgShell, m_oReqMsgHead, strBody);
    }
}

void async_task_promise_notify_if(StepCo20* step) noexcept
{
    if (step)
    {
        step->NotifyEmitCoroutineSuccess();
    }
}

} // namespace net
