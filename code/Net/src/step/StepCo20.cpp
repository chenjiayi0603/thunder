#include "step/StepCo20.hpp"
#include "NetError.hpp"
#include "NetDefine.hpp"
#include <memory>

namespace net
{

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
        m_oAsyncBootstrap.reset();
        OnCoroutineComplete(true);
        return STATUS_CMD_COMPLETED;
    }
    
    // 启动协程
    m_bCoroutineRunning = true;
    
    // 创建协程任务
    auto coroTask = [this]() -> AsyncTask {
        try
        {
            co_await CoroutineMain();
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
    
    // 启动异步任务（必须延长 AsyncTask 生命周期，不可 coroTask(); 临时析构）
    m_oAsyncBootstrap.emplace(coroTask());

    return STATUS_CMD_RUNNING;
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
    if (m_coroHandle && !m_coroHandle.done())
    {
        m_coroHandle.resume();
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
    if (m_coroHandle && !m_coroHandle.done())
    {
        m_coroHandle.resume();
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

Task<bool> StepCo20::HttpGetAsync(const std::string& strUrl)
{
    bool bSuccess = HttpGet(strUrl);
    if (!bSuccess)
    {
        co_return false;
    }
    
    // 等待 HTTP 响应
    HttpRespAwaiter awaiter(this);
    bool bResult = co_await awaiter;
    co_return bResult;
}

Task<bool> StepCo20::HttpPostAsync(const std::string& strUrl, const std::string& strBody)
{
    bool bSuccess = HttpPost(strUrl, strBody);
    if (!bSuccess)
    {
        co_return false;
    }
    
    // 等待 HTTP 响应
    HttpRespAwaiter awaiter(this);
    bool bResult = co_await awaiter;
    co_return bResult;
}

Task<bool> StepCo20::SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg)
{
    bool bSuccess = SendTo(stMsgShell, oHttpMsg);
    if (!bSuccess)
    {
        co_return false;
    }
    
    // 等待响应
    HttpRespAwaiter awaiter(this);
    bool bResult = co_await awaiter;
    co_return bResult;
}

Task<bool> StepCo20::SendToInternalAsync(const tagMsgShell& stMsgShell, MsgHead oMsgHead, MsgBody oMsgBody)
{
    oMsgHead.set_seq(GetSequence());
    oMsgHead.set_msgbody_len(static_cast<uint32_t>(oMsgBody.ByteSizeLong()));
    bool bSuccess = GetLabor()->SendTo(stMsgShell, oMsgHead, oMsgBody);
    if (!bSuccess)
    {
        co_return false;
    }
    HttpRespAwaiter awaiter(this);
    bool bResult = co_await awaiter;
    co_return bResult;
}

Task<bool> StepCo20::SendToInternalByIdentifyAsync(const std::string& strIdentify, MsgHead oMsgHead, MsgBody oMsgBody)
{
    oMsgHead.set_seq(GetSequence());
    oMsgHead.set_msgbody_len(static_cast<uint32_t>(oMsgBody.ByteSizeLong()));
    bool bSuccess = GetLabor()->SendTo(strIdentify, oMsgHead, oMsgBody);
    if (!bSuccess)
    {
        co_return false;
    }
    HttpRespAwaiter awaiter(this);
    bool bResult = co_await awaiter;
    co_return bResult;
}

Task<bool> StepCo20::SendToInternalByNodeTypeAsync(const std::string& strNodeType, MsgHead oMsgHead, MsgBody oMsgBody)
{
    oMsgHead.set_seq(GetSequence());
    oMsgHead.set_msgbody_len(static_cast<uint32_t>(oMsgBody.ByteSizeLong()));
    bool bSuccess = GetLabor()->SendToSession(strNodeType, oMsgHead, oMsgBody);
    if (!bSuccess)
    {
        co_return false;
    }
    HttpRespAwaiter awaiter(this);
    bool bResult = co_await awaiter;
    co_return bResult;
}

void StepCo20::OnCoroutineComplete(bool bSuccess)
{
    LOG4_TRACE("%s() success:%d", __FUNCTION__, bSuccess);
    
    if (bSuccess)
    {
        ResponseToClient(200, "{\"code\":0,\"msg\":\"ok\"}");
    }
    else
    {
        ResponseToClient(500, "{\"code\":1,\"msg\":\"internal error\"}");
    }
}

void StepCo20::OnCoroutineError(int iErrno, const std::string& strErrMsg)
{
    LOG4_ERROR("%s() errno:%d msg:%s", __FUNCTION__, iErrno, strErrMsg.c_str());
    
    m_iErrno = iErrno;
    m_strErrMsg = strErrMsg;
    
    ResponseToClient(500, "{\"code\":" + std::to_string(iErrno) + 
                     ",\"msg\":\"" + strErrMsg + "\"}");
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

} // namespace net