/*******************************************************************************
 * Interface 节点独立副本（见 StepHttpRequestCo20.hpp）
 ******************************************************************************/
#include "StepHttpRequestCo20.hpp"
#include "util/CommonUtils.hpp"

namespace core
{

namespace {
// 避免把超大 HTML 整段塞进 JSON 拖垮客户端/日志
constexpr size_t kMaxUpstreamBodyInClientJson = static_cast<size_t>(64 * 1024);
}

net::Task<> StepHttpRequestCo20::CoroutineMain()
{
    LOG4_TRACE("%s() start", __FUNCTION__);

    try
    {
        // example.com：IETF 保留示例域，响应体仅约数百字节，远小于百度首页（便于回传 upstream_body）
        LOG4_TRACE("%s() request example.com, testVal:%u", __FUNCTION__, ++m_uiTestVal);
        const bool bSuccess = co_await HttpGetAsync("http://example.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://example.com/ error");
            Response(1, nullptr);
            co_return;
        }

        LOG4_TRACE("%s() complete, testVal:%u", __FUNCTION__, ++m_uiTestVal);
        Response(0, &GetLastRspHttpMsg());
    }
    catch (const std::exception& e)
    {
        LOG4_ERROR("%s() exception: %s", __FUNCTION__, e.what());
        Response(1, nullptr);
    }
    catch (...)
    {
        LOG4_ERROR("%s() unknown exception", __FUNCTION__);
        Response(1, nullptr);
    }

    co_return;
}

void StepHttpRequestCo20::Response(int nCode, const HttpMsg* pUpstreamRsp)
{
    HttpMsg oHttpMsg;
    util::CJsonObject oJsonObj;
    oHttpMsg.set_type(HTTP_RESPONSE);
    oHttpMsg.set_status_code(200);
    oHttpMsg.set_http_major(m_oInHttpMsg.http_major());
    oHttpMsg.set_http_minor(m_oInHttpMsg.http_minor());
    oJsonObj.Add("code", nCode);
    oJsonObj.Add("msg", "ok");
    oJsonObj.Add("testVal", m_uiTestVal);
    if (pUpstreamRsp != nullptr)
    {
        oJsonObj.Add("upstream_http_status", static_cast<int32_t>(pUpstreamRsp->status_code()));
        const std::string& raw = pUpstreamRsp->body();
        if (raw.size() <= kMaxUpstreamBodyInClientJson)
        {
            oJsonObj.Add("upstream_body", raw);
            oJsonObj.Add("upstream_body_truncated", false, false);
        }
        else
        {
            oJsonObj.Add("upstream_body", raw.substr(0, kMaxUpstreamBodyInClientJson));
            oJsonObj.Add("upstream_body_truncated", true, true);
        }
    }
    oHttpMsg.set_body(oJsonObj.ToString());
    GetLabor()->SendTo(m_stReqMsgShell, oHttpMsg);
}

} // namespace core
