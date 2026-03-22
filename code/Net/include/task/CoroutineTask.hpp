#pragma once

#include "task/HttpTask.hpp"

namespace net {

class CoroutineTask : public HttpTask {
public:
    CoroutineTask();
    CoroutineTask(const tagMsgShell& shell, const HttpMsg& httpMsg);
    CoroutineTask(const tagMsgShell& shell, const MsgHead& head);
    CoroutineTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body);
    ~CoroutineTask() override = default;
};

} // namespace net
