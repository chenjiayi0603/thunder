#include "task/CoroutineTask.hpp"

namespace net {

CoroutineTask::CoroutineTask(Step* pNextStep) : HttpTask(pNextStep) {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

CoroutineTask::CoroutineTask(const tagMsgShell& shell, const HttpMsg& httpMsg, Step* pNextStep)
    : HttpTask(shell, httpMsg, pNextStep) {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

CoroutineTask::CoroutineTask(const tagMsgShell& shell, const MsgHead& head, Step* pNextStep)
    : HttpTask(shell, head, pNextStep) {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

CoroutineTask::CoroutineTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body,
                             Step* pNextStep)
    : HttpTask(shell, head, body, pNextStep) {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

} // namespace net
