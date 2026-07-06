#pragma once
#include "coro/StepCo20.hpp"
#include <string>

namespace core {

/// #130: HTTPS 出站异步测试 — co_await HttpGetAsync(url)，等回包后返回给客户端
class HttpsTestCo : public net::StepCo20 {
public:
    HttpsTestCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg, const std::string& url)
        : net::StepCo20(stMsgShell, oInHttpMsg), m_strUrl(url) {
        m_strStepDesc = "HttpsTestCo";
    }
    virtual ~HttpsTestCo() = default;
    virtual net::AsyncTask StepAsync() override;
private:
    static net::AsyncTask AsyncBody(net::StepCo20& st);
    std::string m_strUrl;
};

}
