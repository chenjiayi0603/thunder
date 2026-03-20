#pragma once

#include "task/HttpTask.hpp"

namespace net {

class CoroutineTask : public HttpTask {
public:
    CoroutineTask(Step* pNextStep = nullptr);
    CoroutineTask(const tagMsgShell& shell, const HttpMsg& httpMsg, Step* pNextStep = nullptr);
    CoroutineTask(const tagMsgShell& shell, const MsgHead& head, Step* pNextStep = nullptr);
    CoroutineTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body,
                    Step* pNextStep = nullptr);
    ~CoroutineTask() override = default;
};

} // namespace net
