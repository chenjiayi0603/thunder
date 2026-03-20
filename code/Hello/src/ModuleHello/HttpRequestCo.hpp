#ifndef SRC_HttpRequestCo_HPP_
#define SRC_HttpRequestCo_HPP_

#include "step/CoroutineState.hpp"

namespace core
{

/**
 * @brief 使用 C++20 协程的 HTTP 请求示例（基于 CoroutineState）
 * @note 展示如何使用 CoroutineState 替代 StepState 状态机模式
 *       使用同步写法编写异步代码
 */
class HttpRequestCo : public net::CoroutineState
{
public:
    HttpRequestCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
        : net::CoroutineState(stMsgShell, oInHttpMsg)
    {
        m_strStepDesc = "HttpRequestCo";
    }

    virtual ~HttpRequestCo() = default;

    /**
     * @brief 协程主函数
     * @note 使用 co_await 语法编写异步逻辑，看起来像同步代码
     *       将原来的 5 个 State 函数合并为 1 个线性函数
     */
    virtual net::CoTask<> Run() override;

private:
    uint32 m_uiTestVal = 0;
    
    /**
     * @brief 发送响应
     */
    void Response(int nCode);
};

} // namespace core

#endif // SRC_HttpRequestCo_HPP_