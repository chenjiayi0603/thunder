#ifndef SRC_StepCo20_HPP_
#define SRC_StepCo20_HPP_

#include "Coroutine20.hpp"
#include "step/HttpStep.hpp"
#include "NetDefine.hpp"
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
     * @brief 单条最外层 AsyncTask：Emit() 将其 emplace 进 m_oAsyncBootstrap，不在此帧外再 co_await 一层 Task<void>。
     * @note StepCo20Func 的 StepAsync 直接 return 用户 `AsyncTask`；直接继承本类时请在本协程体内 co_await HttpGetAsync 等，
     *       并在正常结束前调用 NotifyEmitCoroutineSuccess()，或使用 EmitSuccessGuard 自动于 co_return 前触发。
     */
    virtual AsyncTask StepAsync() = 0;

    /**
     * @brief 标记协程体已正常跑完，供 Callback 返回 COMPLETED（与旧 Emit 中外层 try 块尾部逻辑一致）。
     */
    void NotifyEmitCoroutineSuccess();

    /**
     * @brief 放在返回 AsyncTask 的协程体**开头**；任意路径执行到 co_return 时，标准会先按块析构局部变量，再
     *        promise.return_void，故本守卫析构中的 Notify 等价于「在 co_return 之前」调用。
     * @note co_await 挂起不会析构本对象。不应与手写 NotifyEmitCoroutineSuccess 叠用（会重复 OnCoroutineComplete）。
     *       若某条路径不应标记完成，可在该路径上先 dismiss()。
     */
    struct EmitSuccessGuard
    {
        StepCo20* step_{};

        explicit EmitSuccessGuard(StepCo20& s) noexcept : step_(&s) {}
        EmitSuccessGuard(const EmitSuccessGuard&) = delete;
        EmitSuccessGuard& operator=(const EmitSuccessGuard&) = delete;
        void dismiss() noexcept { step_ = nullptr; }
        ~EmitSuccessGuard()
        {
            if (step_)
                step_->NotifyEmitCoroutineSuccess();
        }
    };
    
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

    /**
     * @brief 响应客户端（对 StepCo20Func 等传入的 lambda 公开，便于协程体调用）
     */
    void ResponseToClient(int iCode = 200, const std::string& strBody = "");

protected:
    /**
     * @brief 协程完成回调
     */
    virtual void OnCoroutineComplete(bool bSuccess);
    
    /**
     * @brief 协程错误处理
     */
    virtual void OnCoroutineError(int iErrno, const std::string& strErrMsg);

    friend struct HttpRespAwaiter;
    friend struct CoSleepAwaiter;
    friend void CoSleepTimerTrampoline(struct ev_loop*, struct ev_timer*, int);
    friend class RedisAwaitable;

protected:
    std::string m_strStepDesc = "StepCo20";

private:
    /// 包住 StepAsync() 的单条 AsyncTask；不可像临时对象那样在 Emit() 末尾析构，否则挂起后帧被
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
 * @brief 在 Step 协程体内 `co_await HttpRespAwaiter(this)`，把当前协程挂起直到 Worker 回调里 `m_coroHandle.resume()`。
 * @note 触发顺序（相对 `Task::continuation_` 独立，用 Step 侧槽位接异步）：
 *       1. 求值 `co_await` 时构造本 awaiter（构造函数）。
 *       2. `await_ready()`：恒 false，总是进入挂起路径。
 *       3. `await_suspend(handle)`：handle 为**当前正在 co_await 的这条 Step 协程帧**（多为 HttpGetAsync 等子 Task 本体），
 *          写入 `pStep->m_coroHandle`，供 `StepCo20::Callback` 在填好 `m_oResHttpMsg` 等之后 resume。
 *       4. `await_resume()`：`Callback`（或等价路径）已 `resume` 且协程继续执行到 `co_await` 的恢复点时调用；
 *          此时读 `m_oResHttpMsg` 判定成功与否，返回值作为 `co_await` 表达式的结果。
 */
struct HttpRespAwaiter
{
    StepCo20* pStep;

    explicit HttpRespAwaiter(StepCo20* step) : pStep(step) {}

    /// 每次进入该 `co_await` 时由运行时先问；恒 false 表示必须挂起，不能同步继续。
    bool await_ready() const noexcept { return false; }

    /// 父协程（此处即 co_await 所在的那条 Step 协程）即将挂起时调用；handle 为该帧句柄，非 Labor/Worker。
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        pStep->m_coroHandle = handle;
    }

    /// 异步路径已 `m_coroHandle.resume()` 后，在 `co_await` 恢复点调用；HTTP 按 status_code；非 RESPONSE 视为成功。
    bool await_resume() noexcept
    {
        const HttpMsg& rsp = pStep->m_oResHttpMsg;
        if (rsp.type() == HTTP_RESPONSE)
        {
            const int code = rsp.status_code();
            return code >= 200 && code < 400;
        }
        return true;
    }
};

/**
 * @brief 协程体内短时休眠（一次性 ev_timer），供等待内部路由就绪等场景。
 */
struct CoSleepAwaiter
{
    StepCo20* pStep{};
    double delaySec{};

    bool await_ready() const noexcept { return delaySec <= 0.0; }
    void await_suspend(std::coroutine_handle<> handle) noexcept;
    void await_resume() noexcept {}
};

} // namespace net

#endif // SRC_StepCo20_HPP_