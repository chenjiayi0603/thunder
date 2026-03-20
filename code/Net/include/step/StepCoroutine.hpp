/*******************************************************************************
 * Project:  Net
 * @file     StepCoroutine.hpp
 * @brief    C++20 协程步骤基类
 * @author   cjy
 * @date:    2024
 * @note     用于替换 StepState 状态机模式，使异步代码写起来像同步代码
 *           继承自 HttpStep，与 StepState 平级
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_STEPCOROUTINE_HPP_
#define SRC_STEP_STEPCOROUTINE_HPP_

#include <coroutine>
#include <string>
#include <unordered_map>
#include "step/HttpStep.hpp"
#include "step/CoTask.hpp"
#include "step/HttpAwaitable.hpp"

namespace net
{

/**
 * @brief C++20 协程步骤基类
 * @note 继承自 HttpStep，与 StepState 平级
 *       使用 C++20 协程替换状态机模式，使异步代码写起来像同步代码
 * 
 * 类层次结构：
 * HttpStep
 * ├── StepState     (现有，状态机模式)
 * └── StepCoroutine (新增，C++20 协程)
 */
class StepCoroutine : public HttpStep
{
public:
    StepCoroutine();
    StepCoroutine(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead);
    StepCoroutine(const tagMsgShell& stInMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody);
    StepCoroutine(const tagMsgShell& stInMsgShell, const HttpMsg& oInHttpMsg);
    virtual ~StepCoroutine();

    /**
     * @brief 初始化超时参数
     * @param uiTimeOutMax 最大超时次数
     * @param uiTimeOutRetry 是否超时重试
     */
    void Init(uint32 uiTimeOutMax, uint8 uiTimeOutRetry)
    {
        m_uiTimeOutMax = uiTimeOutMax;
        m_uiTimeOutRetry = uiTimeOutRetry;
    }

    /**
     * @brief 设置步骤描述
     */
    virtual void SetStepDesc(const std::string& s)
    {
        m_strStepDesc = s;
    }

    /**
     * @brief 纯虚函数 - 子类必须实现协程体
     * @return CoTask 协程任务
     */
    virtual CoTask Run() = 0;

    /**
     * @brief 启动/恢复协程
     * @param iErrno 错误码
     * @param strErrMsg 错误信息
     * @param strErrShow 展示给用户的错误描述
     * @return 执行状态
     */
    E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "") override;

    /**
     * @brief HTTP 回调
     * @param stMsgShell 消息外壳
     * @param oHttpMsg HTTP 消息
     * @param data 数据指针
     * @return 执行状态
     */
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg, void* data = nullptr) override;

    /**
     * @brief Protobuf 回调
     * @param stMsgShell 消息外壳
     * @param oInMsgHead 消息头
     * @param oInMsgBody 消息体
     * @param data 数据指针
     * @return 执行状态
     */
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, void* data = nullptr) override;

    /**
     * @brief 超时回调
     * @return 执行状态
     */
    E_CMD_STATUS Timeout() override;

    /**
     * @brief 创建 HTTP GET 请求的 Awaitable
     * @param strUrl 请求 URL
     * @return HttpAwaitable
     */
    HttpAwaitable HttpGetAsync(const std::string& strUrl)
    {
        return HttpAwaitable(this, strUrl);
    }

    /**
     * @brief 创建 HTTP POST 请求的 Awaitable
     * @param strUrl 请求 URL
     * @param strBody 请求体
     * @return HttpAwaitable
     */
    HttpAwaitable HttpPostAsync(const std::string& strUrl, const std::string& strBody)
    {
        return HttpAwaitable(this, strUrl, strBody);
    }

    /**
     * @brief 创建带 Headers 的 HTTP POST 请求的 Awaitable
     * @param strUrl 请求 URL
     * @param strBody 请求体
     * @param mapHeaders 请求头
     * @return HttpAwaitable
     */
    HttpAwaitable HttpPostAsync(const std::string& strUrl, const std::string& strBody,
                                const std::unordered_map<std::string, std::string>& mapHeaders)
    {
        return HttpAwaitable(this, strUrl, strBody, mapHeaders);
    }

    /**
     * @brief 执行 HTTP 请求（被 HttpAwaitable 调用）
     * @param awaitable HTTP Awaitable
     * @return 是否成功发起请求
     */
    bool DoHttpRequest(const HttpAwaitable& awaitable);

    /** @brief 最近一次 HTTP 回调的响应（供 HttpAwaitable::await_resume） */
    const HttpMsg& GetLastHttpResponse() const { return m_oResHttpMsg; }

    /**
     * @brief 保存协程句柄（被 HttpAwaitable 调用）
     * @param h 协程句柄
     */
    void SaveCoroutineHandle(std::coroutine_handle<> h)
    {
        m_suspendedHandle = h;
    }

    /**
     * @brief 成功回调钩子
     */
    virtual void OnSucc() {}

    /**
     * @brief 失败回调钩子
     */
    virtual void OnFail() {}

public:
    // 错误信息
    int m_iErrno = 0;
    std::string m_strStepDesc = "StepCoroutine";
    std::string m_strErrMsg;

    // 响应数据
    HttpMsg m_oResHttpMsg;
    MsgHead m_oResMsgHead;
    MsgBody m_oResMsgBody;

protected:
    uint32 m_uiTimeOutCounter = 0;  // 已超时次数
    uint32 m_uiTimeOutMax = 3;      // 最多超时次数
    uint8 m_uiTimeOutRetry = 0;     // 是否超时重新尝试

private:
    CoTask m_coTask;                          // 协程任务
    bool m_coStarted = false;                 // 协程是否已启动
    std::coroutine_handle<> m_suspendedHandle; // 挂起时的协程句柄
};

} /* namespace net */

#endif /* SRC_STEP_STEPCOROUTINE_HPP_ */
