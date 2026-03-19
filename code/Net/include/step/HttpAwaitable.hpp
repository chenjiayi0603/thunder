/*******************************************************************************
 * Project:  Net
 * @file     HttpAwaitable.hpp
 * @brief    HTTP 请求的 Awaitable 类型
 * @author   cjy
 * @date:    2024
 * @note     用于在协程中 co_await HTTP 请求
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_HTTPAWAITABLE_HPP_
#define SRC_STEP_HTTPAWAITABLE_HPP_

#include <coroutine>
#include <string>
#include <unordered_map>
#include "protocol/http.pb.h"

namespace net
{

class StepCoroutine;

/**
 * @brief HTTP 请求类型枚举
 */
enum class HttpRequestType
{
    GET,
    POST
};

/**
 * @brief HTTP 请求的 Awaitable 类型
 * @note 封装 HTTP GET/POST 请求，可在协程中使用 co_await
 */
class HttpAwaitable
{
public:
    /**
     * @brief 构造函数 (GET 请求)
     * @param pStep 所属的 StepCoroutine
     * @param strUrl 请求 URL
     */
    HttpAwaitable(StepCoroutine* pStep, const std::string& strUrl)
        : m_pStep(pStep)
        , m_strUrl(strUrl)
        , m_requestType(HttpRequestType::GET)
    {
    }

    /**
     * @brief 构造函数 (POST 请求)
     * @param pStep 所属的 StepCoroutine
     * @param strUrl 请求 URL
     * @param strBody 请求体
     */
    HttpAwaitable(StepCoroutine* pStep, const std::string& strUrl, const std::string& strBody)
        : m_pStep(pStep)
        , m_strUrl(strUrl)
        , m_strBody(strBody)
        , m_requestType(HttpRequestType::POST)
    {
    }

    /**
     * @brief 构造函数 (带 Headers 的 POST 请求)
     * @param pStep 所属的 StepCoroutine
     * @param strUrl 请求 URL
     * @param strBody 请求体
     * @param mapHeaders 请求头
     */
    HttpAwaitable(StepCoroutine* pStep, const std::string& strUrl, const std::string& strBody,
                  const std::unordered_map<std::string, std::string>& mapHeaders)
        : m_pStep(pStep)
        , m_strUrl(strUrl)
        , m_strBody(strBody)
        , m_mapHeaders(mapHeaders)
        , m_requestType(HttpRequestType::POST)
    {
    }

    /**
     * @brief 检查是否准备好（总是返回 false，需要挂起）
     */
    bool await_ready() const noexcept
    {
        return false;
    }

    /**
     * @brief 挂起时的处理
     * @param h 协程句柄
     * @note 保存句柄到 StepCoroutine，发起 HTTP 请求
     */
    void await_suspend(std::coroutine_handle<> h);

    /**
     * @brief 恢复时的处理
     * @return HTTP 响应消息的引用
     */
    const HttpMsg& await_resume();

    /**
     * @brief 获取请求类型
     */
    HttpRequestType getRequestType() const
    {
        return m_requestType;
    }

    /**
     * @brief 获取 URL
     */
    const std::string& getUrl() const
    {
        return m_strUrl;
    }

    /**
     * @brief 获取请求体
     */
    const std::string& getBody() const
    {
        return m_strBody;
    }

    /**
     * @brief 获取请求头
     */
    const std::unordered_map<std::string, std::string>& getHeaders() const
    {
        return m_mapHeaders;
    }

private:
    StepCoroutine* m_pStep;                                    // 所属的 StepCoroutine
    std::string m_strUrl;                                      // 请求 URL
    std::string m_strBody;                                     // 请求体
    std::unordered_map<std::string, std::string> m_mapHeaders; // 请求头
    HttpRequestType m_requestType;                             // 请求类型
};

} /* namespace net */

#endif /* SRC_STEP_HTTPAWAITABLE_HPP_ */
