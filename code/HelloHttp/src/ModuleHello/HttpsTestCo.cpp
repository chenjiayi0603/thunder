#include "HttpsTestCo.hpp"
#include "util/json/CJsonObject.hpp"

namespace core {

net::AsyncTask HttpsTestCo::StepAsync() {
    return AsyncBody(*this);
}

net::AsyncTask HttpsTestCo::AsyncBody(net::StepCo20& st) {
    auto& self = static_cast<HttpsTestCo&>(st);
    LOG4_INFO("HttpsTestCo: GET %s", self.m_strUrl.c_str());

    bool ok = co_await self.HttpGetAsync(self.m_strUrl);

    HttpMsg oRsp;
    oRsp.set_type(HTTP_RESPONSE);
    oRsp.set_status_code(ok ? 200 : 502);
    oRsp.set_http_major(self.m_oInHttpMsg.http_major());
    oRsp.set_http_minor(self.m_oInHttpMsg.http_minor());
    util::CJsonObject oJson;
    oJson.Add("code", ok ? 0 : 1);
    oJson.Add("msg", ok ? "https_ok" : "https_failed");
    oJson.Add("url", self.m_strUrl);
    oRsp.set_body(oJson.ToString());
    GetLabor()->SendTo(self.m_stReqMsgShell, oRsp);
    co_return;
}

}
