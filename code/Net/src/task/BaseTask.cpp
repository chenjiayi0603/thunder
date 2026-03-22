#include "task/BaseTask.hpp"
#include "labor/Worker.hpp"

namespace net {

BaseTask::BaseTask() : Step() {}

BaseTask::BaseTask(const tagMsgShell& shell) : Step(shell) {}

BaseTask::BaseTask(const tagMsgShell& shell, const MsgHead& head)
    : Step(shell, head) {}

BaseTask::BaseTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body)
    : Step(shell, head, body) {}

BaseTask::~BaseTask() = default;

void BaseTask::notifyRunFinished(bool success) {
    m_coroutineDone = true;
    if (success)
        OnSucc();
    else
        OnFail();
}

AsyncTask BaseTask::runCoroutineThunk(BaseTask* self) {
    try {
        co_await self->Run();
        self->notifyRunFinished(true);
    } catch (...) {
        self->notifyRunFinished(false);
    }
}

void BaseTask::ResumeCoroutineHandle() {
    if (!m_coroHandle) return;
    auto h = m_coroHandle;
    m_coroHandle = nullptr;
    if (h && !h.done()) h.resume();
}

E_CMD_STATUS BaseTask::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow) {
    (void)iErrno;
    (void)strErrMsg;
    (void)strErrShow;
    if (!m_bCoroutineStarted) {
        m_bCoroutineStarted = true;
        m_cancelToken.Reset();
        m_coroutineDone = false;
        runCoroutineThunk(this);
    }
    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS BaseTask::Callback(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                                  const MsgBody& oInMsgBody, void* data) {
    (void)data;
    ResumeWithResponse(stMsgShell, oInMsgHead, oInMsgBody);
    return m_coroutineDone ? STATUS_CMD_COMPLETED : STATUS_CMD_RUNNING;
}

E_CMD_STATUS BaseTask::Timeout() {
    GetCancellationToken().Cancel();
    OnTimeout();
    return STATUS_CMD_COMPLETED;
}

void BaseTask::OnTimeout() {
    ++m_uiTimeOutCounter;
    if (m_uiTimeOutCounter < m_uiTimeOutMax) {
        return;
    }
    GetCancellationToken().Cancel();
}

void BaseTask::ResumeWithResponse(const tagMsgShell&, const MsgHead& head, const MsgBody& body) {
    if (GetCancellationToken().IsCancelled()) return;
    if (GetLabor()) {
        SetActiveTime(GetLabor()->GetTimeStamp());
    }
    m_oResMsgHead = head;
    m_oResMsgBody = body;
    ResumeCoroutineHandle();
}

void BaseTask::ResumeWithHttpResponse(const tagMsgShell&, const HttpMsg& httpMsg) {
    if (GetCancellationToken().IsCancelled()) return;
    if (GetLabor()) {
        SetActiveTime(GetLabor()->GetTimeStamp());
    }
    m_oResHttpMsg = httpMsg;
    ResumeCoroutineHandle();
}

void BaseTask::ResumeWithError(int errNo, const std::string& errMsg) {
    m_iErrno = errNo;
    m_strErrMsg = errMsg;
    ResumeCoroutineHandle();
}

} // namespace net
