#ifndef SRC_StepCo20_HPP_
#define SRC_StepCo20_HPP_

#include "Coroutine20.hpp"
#include "step/HttpStep.hpp"
#include "NetDefine.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace net
{

template <class BodyT, class OutT, class WorkFn>
struct ThreadPoolAwaiter;
class StepCo20;
struct Context;

class LibevIo
{
public:
    class IoBoolAwaitable;

    LibevIo() = default;
    explicit LibevIo(StepCo20* pStep) : pStep_(pStep) {}

    IoBoolAwaitable HttpGetAsync(const std::string& strUrl);
    IoBoolAwaitable HttpPostAsync(const std::string& strUrl, const std::string& strBody);
    IoBoolAwaitable SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg);
    IoBoolAwaitable SendToInternalAsync(const tagMsgShell& stMsgShell, MsgHead oMsgHead, MsgBody oMsgBody);
    IoBoolAwaitable SendToInternalByIdentifyAsync(const std::string& strIdentify, MsgHead oMsgHead, MsgBody oMsgBody);
    IoBoolAwaitable SendToInternalByNodeTypeAsync(const std::string& strNodeType, MsgHead oMsgHead, MsgBody oMsgBody);

protected:
    StepCo20* pStep_{nullptr};
};

/**
 * @brief 协程上下文（持有恢复句柄 + Libev IO 能力）
 */
struct Context : public LibevIo
{
    explicit Context(StepCo20* pStep = nullptr) : LibevIo(pStep) {}
    std::coroutine_handle<> handle{};
};

/**
 * @brief C++20 协程步骤基类（继承 HttpStep）
 * @note 同步写法、异步调度：
 *       - HTTP：HttpGetAsync / HttpPostAsync / SendToAsync(HttpMsg)，回调走 Callback(HttpMsg)。
 *       - 节点间二进制（PB/内部协议）：SendToInternalAsync / SendToInternalByIdentifyAsync，
 *         发出前将 MsgHead.seq 置为本 Step 的 GetSequence()，与 Worker 按 seq 将响应路由回
 *         Callback(MsgHead, MsgBody) 的机制一致；co_await 后读 GetLastRspMsgHead() / Body()。
 *       - 统一 `IoBoolAwaitable` 表示一次 I/O 等待（发起后挂起，回调恢复并返回 bool）。
 */
class StepCo20 : public HttpStep
{
public:
    using IoBoolAwaitable = LibevIo::IoBoolAwaitable;
    StepCo20() : m_context(this) {}
    StepCo20(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead)
        : HttpStep(stInMsgShell, oInMsgHead), m_context(this) {}
    StepCo20(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
        : HttpStep(stInMsgShell, oInMsgHead, oInMsgBody), m_context(this) {}
    StepCo20(const tagMsgShell& stInMsgShell, const HttpMsg& oInHttpMsg)
        : HttpStep(stInMsgShell, oInHttpMsg), m_context(this) {}
    
    virtual ~StepCo20() = default;

    /**
     * @brief 启动协程步骤
     * @return 执行状态
     */
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", 
                              const std::string& strErrShow = "") override;
    
    /**
     * @brief 单条最外层 AsyncTask：Emit() 将其 emplace 进 m_oAsyncBootstrap 并持有到 Step 生命周期结束。
     * @note 协程首参须为 `StepCo20&`（可有更多形参）；`StepAsync()` 内 `return Body(*this)` 或 `return Body(*this,...)`。
     *       勿用 lambda 直接写 co_await；LaunchCo 宜 `return 具名协程(step,...)`。
     */
    virtual AsyncTask StepAsync() = 0;

    /**
     * @brief 标记协程体已正常跑完，供 Callback 返回 COMPLETED（与旧 Emit 中外层 try 块尾部逻辑一致）。
     */
    void NotifyEmitCoroutineSuccess();

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
    
    /// 异步 HTTP GET，co_await 后返回 bool（统一 IoBoolAwaitable）
    IoBoolAwaitable HttpGetAsync(const std::string& strUrl);
    
    /// 异步 HTTP POST，co_await 后返回 bool（统一 IoBoolAwaitable）
    IoBoolAwaitable HttpPostAsync(const std::string& strUrl, const std::string& strBody);
    
    /// 异步发送 HTTP 消息，co_await 后返回 bool（统一 IoBoolAwaitable）
    IoBoolAwaitable SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg);

    /// 发送内部二进制协议并等待响应，co_await 后返回 bool
    IoBoolAwaitable SendToInternalAsync(const tagMsgShell& stMsgShell, MsgHead oMsgHead, MsgBody oMsgBody);

    /// 按 identify 发送内部二进制协议并等待响应，co_await 后返回 bool
    IoBoolAwaitable SendToInternalByIdentifyAsync(const std::string& strIdentify, MsgHead oMsgHead, MsgBody oMsgBody);
    /// 按 nodeType 发送内部二进制协议并等待响应（含短轮询重试），co_await 后返回 bool
    IoBoolAwaitable SendToInternalByNodeTypeAsync(const std::string& strNodeType, MsgHead oMsgHead, MsgBody oMsgBody);

    const MsgHead& GetLastRspMsgHead() const { return m_oResMsgHead; }
    const MsgBody& GetLastRspMsgBody() const { return m_oResMsgBody; }
    const HttpMsg& GetLastRspHttpMsg() const { return m_oResHttpMsg; }

    bool IsCoroutineCompleted() const noexcept { return m_bCoroutineCompleted; }

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

    friend struct Context;
    friend struct HttpRespAwaiter;
    friend struct CoSleepAwaiter;
    friend void CoSleepTimerTrampoline(struct ev_loop*, struct ev_timer*, int);
    friend class LibevIo::IoBoolAwaitable;
    friend class RedisAwaitable;
    friend class MySqlAwaitable;
    template <class BodyT, class OutT, class WorkFn>
    friend struct ThreadPoolAwaiter;

protected:
    std::string m_strStepDesc = "StepCo20";

private:
    /// 包住 StepAsync() 的单条 AsyncTask；不可像临时对象那样在 Emit() 末尾析构，否则挂起后帧被
    /// destroy，Callback 里 resume 的内层协程完成后会回到已销毁外层（崩溃 / curl 52）。
    std::optional<AsyncTask> m_oAsyncBootstrap;

    /// 当前“待恢复”的协程帧句柄（由 await_suspend(handle) 写入）。
    /// 仅保存恢复入口，不拥有协程生命周期；生命周期由 m_oAsyncBootstrap 持有。
    Context m_context;
    
    // 超时参数
    uint32 m_uiTimeOutCounter = 0;
    uint32 m_uiTimeOutMax = 3;
    uint8 m_uiTimeOutRetry = 0;
    
    // 协程状态
    bool m_bCoroutineRunning = false;
    bool m_bCoroutineCompleted = false;

    // MySQL 桥接取消令牌：await_suspend 创建，StepCo20 超时时设为 true，
    // MySqlStepBridge 检查后跳过 resume（防止访问已销毁的协程帧）。
    std::shared_ptr<bool> m_mysqlCancelToken;
    
    // 响应数据
    HttpMsg m_oResHttpMsg;
    MsgHead m_oResMsgHead;
    MsgBody m_oResMsgBody;
    
    // 错误信息
    int m_iErrno = 0;
    std::string m_strErrMsg;
};

class LibevIo::IoBoolAwaitable
{
public:
    IoBoolAwaitable(StepCo20* pStep,
                    std::function<bool()> startFn);

    bool await_ready() const noexcept { return false; }
    /**
     * @brief 编译器在 `co_await` 展开时，把当前协程帧句柄传入 await_suspend(handle)。
     * @note 保存到 `StepCo20::m_context.handle`，后续在 Callback/Timer 中 resume 回该帧。
     */
    void await_suspend(std::coroutine_handle<> handle) noexcept;
    bool await_resume() noexcept;

private:
    void StartOrFail() noexcept;

    StepCo20* pStep_{nullptr};
    std::function<bool()> startFn_;
    bool forceResultReady_{false};
    bool forceResult_{false};
};

struct HttpRespAwaiter
{
    StepCo20* pContext{};

    explicit HttpRespAwaiter(StepCo20* pStep) : pContext(pStep) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        pContext->m_context.handle = handle;
    }
    bool await_resume() noexcept
    {
        const HttpMsg& rsp = pContext->GetLastRspHttpMsg();
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