/*******************************************************************************
 * Interface 节点独立副本（不与 Hello 共用源文件；逻辑可与 Hello 对齐，自行演进）
 ******************************************************************************/
#ifndef INTERFACE_MODULEHELLO_StepHttpRequestCo20_HPP_
#define INTERFACE_MODULEHELLO_StepHttpRequestCo20_HPP_

#include "step/StepCo20.hpp"

namespace core
{

/**
 * @brief 使用 C++20 协程的 HTTP 请求步骤示例（StepCo20）
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

    virtual net::Task<> CoroutineMain() override;

    void Response(int nCode);

private:
    uint32 m_uiTestVal = 0;
};

} // namespace core

#endif // INTERFACE_MODULEHELLO_StepHttpRequestCo20_HPP_
