#include "step/CoroutineState.hpp"
#include "NetError.hpp"
#include "NetDefine.hpp"
#include <memory>

namespace net
{

E_CMD_STATUS CoroutineState::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
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
        OnCoroutineComplete(true);
        return STATUS_CMD_COMPLETED;
    }

    // 启动协程
    m_bCoroutineRunning = true;

    // 创建协程任务
    auto coroTask = [this]() -> AsyncTask {
        try
        {
            co_await Run();  // 调用派生类的 Run() 函数
            m_bCoroutineCompleted = true;
            m_bCoroutineRunning = false;

            // 协程完成后，需要重新调度 Emit 来处理完成状态
            GetLabor()->ExecStep(this, 0.0);
        }
        catch (const std::exception& e)
        {
            LOG4_ERROR("%s() coroutine exception: %s", __FUNCTION__, e.what());
            m_bCoroutineRunning = false;
            OnCoroutineError(ERR_UNKNOWN_CMD, e.what());
        }
        catch (...)
        {
            LOG4_ERROR("%s() unknown coroutine exception", __FUNCTION__);
            m_bCoroutineRunning = false;
            OnCoroutineError(ERR_UNKNOWN_CMD, "unknown coroutine exception");
        }
    };

    // 启动异步任务
    coroTask();

    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS CoroutineState::Callback(const tagMsgShell& stMsgShell,
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
    if (m_coroHandle && !m_coroHandle.done())
    {
        m_coroHandle.resume();
    }

    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS CoroutineState::Callback(const tagMsgShell& stMsgShell,
                                      const HttpMsg& oHttpMsg,
                                      void* data)
{
    LOG4_TRACE("%s()", __FUNCTION__);

    if (CMD_RSP_SYS_ERROR == oHttpMsg.status_code())
    {
        LOG4_ERROR("system response error");
        return STATUS_CMD_FAULT;
    }

    m_oResHttpMsg = oHttpMsg;

    // 重置超时计数器
    m_uiTimeOutCounter = 0;

    // 恢复协程执行
    if (m_coroHandle && !m_coroHandle.done())
    {
        m_coroHandle.resume();
    }

    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS CoroutineState::Timeout()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ++m_uiTimeOutCounter;
    if (m_uiTimeOutCounter < m_uiTimeOutMax)
    {
        LOG4_WARN("%s() timeout %u times, max %u", __FUNCTION__, m_uiTimeOutCounter, m_uiTimeOutMax);
        return STATUS_CMD_RUNNING;
    }
    LOG4_ERROR("%s() timeout exceed max %u", __FUNCTION__, m_uiTimeOutMax);
    OnCoroutineError(ERR_TIMEOUT, "operation timeout");
    return STATUS_CMD_FAULT;
}

CoTask<bool> CoroutineState::HttpGetAsync(const std::string& strUrl)
{
    LOG4_TRACE("%s() url:%s", __FUNCTION__, strUrl.c_str());

    // 创建 HTTP 请求（与 HttpStep::HttpGet 字段一致）
    HttpMsg oHttpMsg;
    oHttpMsg.set_http_major(1);
    oHttpMsg.set_http_minor(1);
    oHttpMsg.set_type(HTTP_REQUEST);
    oHttpMsg.set_method(HTTP_GET);
    oHttpMsg.set_url(strUrl);
    oHttpMsg.set_status_code(200);

    // 发送请求
    if (SendTo(m_stReqMsgShell, oHttpMsg))
    {
        // 等待响应
        co_await WaitForAsync();
        co_return true;
    }
    else
    {
        LOG4_ERROR("send http get request failed");
        co_return false;
    }
}

CoTask<bool> CoroutineState::HttpPostAsync(const std::string& strUrl, const std::string& strBody)
{
    LOG4_TRACE("%s() url:%s, body size:%zu", __FUNCTION__, strUrl.c_str(), strBody.size());

    HttpMsg oHttpMsg;
    oHttpMsg.set_http_major(1);
    oHttpMsg.set_http_minor(1);
    oHttpMsg.set_type(HTTP_REQUEST);
    oHttpMsg.set_method(HTTP_POST);
    oHttpMsg.set_url(strUrl);
    oHttpMsg.set_status_code(200);
    oHttpMsg.set_body(strBody);

    // 发送请求
    if (SendTo(m_stReqMsgShell, oHttpMsg))
    {
        // 等待响应
        co_await WaitForAsync();
        co_return true;
    }
    else
    {
        LOG4_ERROR("send http post request failed");
        co_return false;
    }
}

CoTask<bool> CoroutineState::SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);

    // 发送消息
    if (SendTo(stMsgShell, oHttpMsg))
    {
        // 等待响应
        co_await WaitForAsync();
        co_return true;
    }
    else
    {
        LOG4_ERROR("send message failed");
        co_return false;
    }
}

void CoroutineState::OnCoroutineComplete(bool bSuccess)
{
    // 业务协程（如 HttpRequestCo::Run）通常已自行 Response；此处不再默认回包，避免重复发送
    LOG4_TRACE("%s() success:%d (no default body; override if needed)", __FUNCTION__, static_cast<int>(bSuccess));
    (void)bSuccess;
}

void CoroutineState::OnCoroutineError(int iErrno, const std::string& strErrMsg)
{
    LOG4_ERROR("%s() errno:%d, errmsg:%s", __FUNCTION__, iErrno, strErrMsg.c_str());

    m_iLastErrno = iErrno;
    m_strLastErrMsg = strErrMsg;

    // 发送错误响应
    if (m_stReqMsgShell.iFd > 0)
    {
        HttpMsg oHttpMsg;
        oHttpMsg.set_http_major(1);
        oHttpMsg.set_http_minor(1);
        oHttpMsg.set_status_code(500);
        oHttpMsg.set_body("{\"code\":" + std::to_string(iErrno) + ",\"msg\":\"" + strErrMsg + "\"}");

        SendTo(m_stReqMsgShell, oHttpMsg);
    }
}

void CoroutineState::StartCoroutine()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    // 协程启动逻辑已经在 Emit 中实现
}

void CoroutineState::ResumeCoroutine()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    
    if (m_coroHandle && !m_coroHandle.done())
    {
        m_coroHandle.resume();
    }
}

void CoroutineState::SetCoroutineHandle(std::coroutine_handle<> handle)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    m_coroHandle = handle;
}

CoTask<void> CoroutineState::WaitForAsync()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    
    // 创建一个可等待对象，用于挂起协程
    struct AsyncAwaiter
    {
        CoroutineState* m_pState;
        
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<> handle) noexcept
        {
            // 保存协程句柄，等待回调时恢复
            m_pState->SetCoroutineHandle(handle);
        }
        
        void await_resume() noexcept {}
    };
    
    co_await AsyncAwaiter{this};
}

} // namespace net