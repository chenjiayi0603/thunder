/*******************************************************************************
 * Interface 节点独立副本（不与 Hello 共用源文件；逻辑可与 Hello 对齐，自行演进）
 ******************************************************************************/
#ifndef INTERFACE_MODULEHELLO_HttpRequestCo_HPP_
#define INTERFACE_MODULEHELLO_HttpRequestCo_HPP_

#include "step/CoroutineState.hpp"

namespace core
{

/**
 * @brief 使用 C++20 协程的 HTTP 请求示例（基于 CoroutineState）
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

    virtual net::CoTask<void> Run() override;

private:
    uint32 m_uiTestVal = 0;

    void Response(int nCode);
};

} // namespace core

#endif // INTERFACE_MODULEHELLO_HttpRequestCo_HPP_
