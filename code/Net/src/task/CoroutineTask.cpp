#include "task/CoroutineTask.hpp"

namespace net {

CoroutineTask::CoroutineTask() : HttpTask() {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

CoroutineTask::CoroutineTask(const tagMsgShell& shell, const HttpMsg& httpMsg)
    : HttpTask(shell, httpMsg) {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

CoroutineTask::CoroutineTask(const tagMsgShell& shell, const MsgHead& head)
    : HttpTask(shell, head) {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

CoroutineTask::CoroutineTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body)
    : HttpTask(shell, head, body) {
    SetTaskDesc("CoroutineTask");
    SetClassName("CoroutineTask");
}

} // namespace net
