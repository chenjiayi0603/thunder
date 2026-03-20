#pragma once

#include "step/Coroutine20.hpp"
#include "task/CancellationToken.hpp"
#include "task/TaskParam.hpp"
#include "NetError.hpp"
#include "NetDefine.hpp"
#include "labor/Labor.hpp"
#include "cmd/CW.hpp"
#include <coroutine>
#include <string>

namespace net {

class Worker;

/**
 * @brief 协程化异步任务基类（过渡期继承 Step，与 Worker::ExecStep 兼容）
 */
class BaseTask : public Step {
public:
    BaseTask(Step* pNextStep = nullptr);
    BaseTask(const tagMsgShell& shell, Step* pNextStep = nullptr);
    BaseTask(const tagMsgShell& shell, const MsgHead& head, Step* pNextStep = nullptr);
    BaseTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body, Step* pNextStep = nullptr);
    ~BaseTask() override;

    virtual Task<void> Run() = 0;

    E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "",
                      const std::string& strErrShow = "") override;
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                          const MsgBody& oInMsgBody, void* data = nullptr) override;
    E_CMD_STATUS Timeout() override;

    void ResumeWithResponse(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body);
    void ResumeWithHttpResponse(const tagMsgShell& shell, const HttpMsg& httpMsg);
    void ResumeWithError(int errNo, const std::string& errMsg);

    void SaveCoroutineHandle(std::coroutine_handle<> h) { m_coroHandle = h; }

    CancellationToken& GetCancellationToken() { return m_cancelToken; }

    bool IsCoroutineDone() const { return m_coroutineDone; }

    virtual void OnTimeout();
    void SetTimeoutParams(uint32 maxRetry = 3, uint8 retryOnTimeout = 0) {
        m_uiTimeOutMax = maxRetry;
        m_uiTimeOutRetry = retryOnTimeout;
    }

    virtual void OnSucc() {}
    virtual void OnFail() {}

    void SetTaskDesc(const std::string& s) { m_strTaskDesc = s; }
    const std::string& GetTaskDesc() const { return m_strTaskDesc; }

    HttpMsg m_oResHttpMsg;
    MsgHead m_oResMsgHead;
    MsgBody m_oResMsgBody;
    int m_iErrno = 0;
    std::string m_strErrMsg;

    const HttpMsg& GetLastHttpMsg() const { return m_oResHttpMsg; }
    int GetLastErrno() const { return m_iErrno; }
    const std::string& GetLastErrMsg() const { return m_strErrMsg; }
    void ClearPendingError() {
        m_iErrno = 0;
        m_strErrMsg.clear();
    }

protected:
    std::string m_strTaskDesc = "BaseTask";

    uint32 m_uiTimeOutCounter = 0;
    uint32 m_uiTimeOutMax = 3;
    uint8 m_uiTimeOutRetry = 0;

    void ResumeCoroutineHandle();

private:
    bool m_coroutineDone = false;

    void notifyRunFinished(bool success);
    static AsyncTask runCoroutineThunk(BaseTask* self);
    std::coroutine_handle<> m_coroHandle;
    bool m_bCoroutineStarted = false;
    CancellationToken m_cancelToken;

    friend class Worker;
};

} // namespace net
