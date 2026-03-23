#ifndef SRC_HttpRequestCo_HPP_
#define SRC_HttpRequestCo_HPP_

#include "coro/StepCo20.hpp"

namespace core
{

/**
 * @brief 使用 C++20 协程的 HTTP 请求示例（基于 StepCo20）
 * @note 与 StepHttpRequestCo 同为协程写法演示；JSON 中 stepType 区分标识
 */
class HttpRequestCo : public net::StepCo20
{
public:
    HttpRequestCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
        : net::StepCo20(stMsgShell, oInHttpMsg)
    {
        m_strStepDesc = "HttpRequestCo";
    }

    virtual ~HttpRequestCo() = default;

    virtual net::AsyncTask StepAsync() override;

private:
    /// 协程第一形参 StepCo20& → promise_type(StepCo20&)；静态成员可访问本类私有成员
    static net::AsyncTask AsyncBody(net::StepCo20& st);

    uint32 m_uiTestVal = 0;

    void Response(int nCode);
};

} // namespace core

#endif // SRC_HttpRequestCo_HPP_
