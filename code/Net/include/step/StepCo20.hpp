#ifndef SRC_StepCo20_HPP_
#define SRC_StepCo20_HPP_

#include "Coroutine20.hpp"
#include "Step.hpp"
#include "HttpStep.hpp"
#include "NetDefine.hpp"
#include <functional>
#include <memory>
#include <optional>

namespace net
{

/**
 * @brief C++20 协程步骤基类（继承 HttpStep）
 * @note 同步写法、异步调度：
 *       - HTTP：HttpGetAsync / HttpPostAsync / SendToAsync(HttpMsg)，回调走 Callback(HttpMsg)。
 *       - 节点间二进制（PB/内部协议）：SendToInternalAsync / SendToInternalByIdentifyAsync，
 *         发出前将 MsgHead.seq 置为本 Step 的 GetSequence()，与 Worker 按 seq 将响应路由回
 *         Callback(MsgHead, MsgBody) 的机制一致；co_await 后读 GetLastRspMsgHead() / Body()。
 *       - HttpRespAwaiter 实际表示「任意一次回调」后的恢复（HTTP 或二进制）。
 */
class StepCo20 : public HttpStep
{
public:
    StepCo20() = default;
    StepCo20(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead)
        : HttpStep(stInMsgShell, oInMsgHead) {}
    StepCo20(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
        : HttpStep(stInMsgShell, oInMsgHead, oInMsgBody) {}
    StepCo20(const tagMsgShell& stInMsgShell, const HttpMsg& oInHttpMsg)
        : HttpStep(stInMsgShell, oInHttpMsg) {}
    
    virtual ~StepCo20() = default;

    /**
     * @brief 启动协程步骤
     * @return 执行状态
     */
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", 
                              const std::string& strErrShow = "") override;
    
    /**
     * @brief 协程主函数
     * @note 派生类需要重写此函数实现协程逻辑
     */
    virtual Task<> CoroutineMain() = 0;
    
    /**
     * @brief 步骤回调函数
     */
    virtual E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                                  const MsgHead& oInMsgHead,
                                  const MsgBody& oInMsgBody,
                                  void* data = nullptr) override;
    
    /**
     * @brief HTTP 回调函数
     */
    virtual E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                                  const HttpMsg& oHttpMsg,
                                  void* data = nullptr) override;
    
    /**
     * @brief 步骤超时回调
     */
    virtual E_CMD_STATUS Timeout() override;
    
    /**
     * @brief 设置超时参数
     */
    void SetTimeoutParams(uint32 uiTimeOutMax = 3, uint8 uiTimeOutRetry = 0)
    {
        m_uiTimeOutMax = uiTimeOutMax;
        m_uiTimeOutRetry = uiTimeOutRetry;
    }
    
    /**
     * @brief 异步 HTTP GET 请求
     * @return Task<bool> 表示请求是否成功发起
     */
    Task<bool> HttpGetAsync(const std::string& strUrl);
    
    /**
     * @brief 异步 HTTP POST 请求
     * @return Task<bool> 表示请求是否成功发起
     */
    Task<bool> HttpPostAsync(const std::string& strUrl, const std::string& strBody);
    
    /**
     * @brief 异步发送数据
     * @return Task<bool> 表示发送是否成功
     */
    Task<bool> SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg);

    /**
     * @brief 向指定连接异步发送内部二进制协议（MsgHead+MsgBody），并挂起直到本次请求的响应回调
     * @note 自动设置 oMsgHead.seq = GetSequence()、msgbody_len，以便 Worker 按 seq 回调本 Step
     */
    Task<bool> SendToInternalAsync(const tagMsgShell& stMsgShell, MsgHead oMsgHead, MsgBody oMsgBody);

    /**
     * @brief 向 strIdentify 对应节点异步发送内部二进制协议（与 Labor::SendTo(identify,...) 一致）
     */
    Task<bool> SendToInternalByIdentifyAsync(const std::string& strIdentify, MsgHead oMsgHead, MsgBody oMsgBody);
    /**
     * @brief 向 strNodeType 对应节点异步发送内部二进制协议（与 Labor::SendToSession(nodeType,...) 一致）
     */
    Task<bool> SendToInternalByNodeTypeAsync(const std::string& strNodeType, MsgHead oMsgHead, MsgBody oMsgBody);

    const MsgHead& GetLastRspMsgHead() const { return m_oResMsgHead; }
    const MsgBody& GetLastRspMsgBody() const { return m_oResMsgBody; }
    const HttpMsg& GetLastRspHttpMsg() const { return m_oResHttpMsg; }

protected:
    /**
     * @brief 协程完成回调
     */
    virtual void OnCoroutineComplete(bool bSuccess);
    
    /**
     * @brief 协程错误处理
     */
    virtual void OnCoroutineError(int iErrno, const std::string& strErrMsg);
    
    /**
     * @brief 响应客户端
     */
    void ResponseToClient(int iCode = 200, const std::string& strBody = "");

    friend struct HttpRespAwaiter;

protected:
    std::string m_strStepDesc = "StepCo20";

private:
    /// 包住「co_await CoroutineMain()」的外层 AsyncTask；不可像临时对象那样在 Emit() 末尾析构，否则挂起后帧被
    /// destroy，Callback 里 resume 的内层协程完成后会回到已销毁外层（崩溃 / curl 52）。
    std::optional<AsyncTask> m_oAsyncBootstrap;

    // 协程句柄
    std::coroutine_handle<> m_coroHandle;
    
    // 超时参数
    uint32 m_uiTimeOutCounter = 0;
    uint32 m_uiTimeOutMax = 3;
    uint8 m_uiTimeOutRetry = 0;
    
    // 协程状态
    bool m_bCoroutineRunning = false;
    bool m_bCoroutineCompleted = false;
    
    // 响应数据
    HttpMsg m_oResHttpMsg;
    MsgHead m_oResMsgHead;
    MsgBody m_oResMsgBody;
    
    // 错误信息
    int m_iErrno = 0;
    std::string m_strErrMsg;
};

/**
 * @brief 异步一步回调等待器（HTTP 或内部二进制响应）
 */
struct HttpRespAwaiter
{
    StepCo20* pStep;
    bool bSuccess;
    
    HttpRespAwaiter(StepCo20* step, bool success) : pStep(step), bSuccess(success) {}
    
    bool await_ready() const noexcept { return false; }
    
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        // 保存协程句柄，等待回调时恢复
        pStep->m_coroHandle = handle;
    }
    
    bool await_resume() noexcept { return bSuccess; }
};

} // namespace net

#endif // SRC_StepCo20_HPP_