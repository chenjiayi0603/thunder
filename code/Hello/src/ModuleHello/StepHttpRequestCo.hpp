#ifndef SRC_StepHttpRequestCo_HPP_
#define SRC_StepHttpRequestCo_HPP_

#include "coro/StepCo20.hpp"

namespace core
{

/**
 * @brief 使用 C++20 协程的 HTTP 请求步骤示例
 * @note 展示如何使用 StepCo20 编写异步 HTTP Step（无需手写状态向量）
 */
class StepHttpRequestCo : public net::StepCo20
{
public:
    StepHttpRequestCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
        : net::StepCo20(stMsgShell, oInHttpMsg)
    {
        m_strStepDesc = "StepHttpRequestCo";
    }

    virtual ~StepHttpRequestCo() = default;

    virtual net::Task<> CoroutineMain() override;

    void Response(int nCode);

private:
    uint32 m_uiTestVal = 0;
};

} // namespace core

#endif // SRC_StepHttpRequestCo_HPP_
