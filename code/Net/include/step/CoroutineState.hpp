#ifndef SRC_CoroutineState_HPP_
#define SRC_CoroutineState_HPP_

#include "Coroutine20.hpp"
#include "Step.hpp"
#include "HttpStep.hpp"
#include "Awaitable.hpp"
#include "RedisAwaitable.hpp"
#include "NetDefine.hpp"
#include <functional>
#include <memory>

namespace net
{

/**
 * @brief C++20 协程状态基类（过渡期）
 * @note 继承自 Step，保持现有框架上下文，提供协程化接口
 *       目标：平滑迁移，最终去掉 Step 概念
 */
class CoroutineState : public HttpStep
{
public:
    CoroutineState() = default;
    CoroutineState(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead)
        : HttpStep(stInMsgShell, oInMsgHead) {}
    CoroutineState(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
        : HttpStep(stInMsgShell, oInMsgHead, oInMsgBody) {}
    CoroutineState(const tagMsgShell& stInMsgShell, const HttpMsg& oInHttpMsg)
        : HttpStep(stInMsgShell, oInHttpMsg) {}
    
    virtual ~CoroutineState() = default;

    /**
     * @brief 启动协程
     * @return 执行状态
     */
    virtual E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", 
                              const std::string& strErrShow = "") override;
    
    /**
     * @brief 协程主函数（纯虚函数）
     * @note 派生类需要重写此函数实现协程逻辑
     *       使用同步写法编写异步代码
     */
    virtual CoTask Run() = 0;
    
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
     * @return CoTask<bool> 表示请求是否成功发起
     */
    CoTask<bool> HttpGetAsync(const std::string& strUrl);
    
    /**
     * @brief 异步 HTTP POST 请求
     * @return CoTask<bool> 表示请求是否成功发起
     */
    CoTask<bool> HttpPostAsync(const std::string& strUrl, const std::string& strBody);
    
    /**
     * @brief 异步发送数据
     * @return CoTask<bool> 表示发送是否成功
     */
    CoTask<bool> SendToAsync(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg);
    
    /**
     * @brief 获取 Labor 上下文
     */
    Labor* GetLabor() const { return GetLaborPtr(); }
    
    /**
     * @brief 获取编解码器
     */
    Codec* GetCodec() const { return GetCodecPtr(); }
    
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
     * @brief 启动协程执行
     */
    void StartCoroutine();
    
    /**
     * @brief 恢复协程执行
     */
    void ResumeCoroutine();
    
    /**
     * @brief 设置协程句柄
     */
    void SetCoroutineHandle(std::coroutine_handle<> handle);
    
    /**
     * @brief 等待异步操作完成
     * @note 内部使用，用于挂起协程等待事件
     */
    CoTask<void> WaitForAsync();
    
private:
    std::coroutine_handle<> m_coroHandle;
    uint32 m_uiTimeOutMax = 3;
    uint8 m_uiTimeOutRetry = 0;
    bool m_bCoroutineRunning = false;
    bool m_bCoroutineCompleted = false;
    int m_iLastErrno = 0;
    std::string m_strLastErrMsg;
};

// 为兼容性定义 CoTask 类型别名
template<typename T = void>
using CoTask = Task<T>;

} // namespace net

#endif // SRC_CoroutineState_HPP_