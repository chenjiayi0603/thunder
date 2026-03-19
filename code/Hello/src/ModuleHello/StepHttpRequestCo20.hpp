#ifndef SRC_StepHttpRequestCo20_HPP_
#define SRC_StepHttpRequestCo20_HPP_

#include "step/StepCo20.hpp"

namespace core
{

/**
 * @brief 使用 C++20 协程的 HTTP 请求步骤示例
 * @note 展示如何使用 StepCo20 替代 StepState 状态机模式
 */
class StepHttpRequestCo20 : public net::StepCo20
{
public:
    StepHttpRequestCo20(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
        : net::StepCo20(stMsgShell, oInHttpMsg)
    {
        m_strStepDesc = "StepHttpRequestCo20";
    }
    
    virtual ~StepHttpRequestCo20() = default;
    
    /**
     * @brief 协程主函数
     * @note 使用 co_await 语法编写异步逻辑，看起来像同步代码
     */
    virtual net::Task<> CoroutineMain() override;
    
private:
    uint32 m_uiTestVal = 0;
};

} // namespace core

#endif // SRC_StepHttpRequestCo20_HPP_