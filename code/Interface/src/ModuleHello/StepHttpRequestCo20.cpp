/*******************************************************************************
 * Interface 节点独立副本（见 StepHttpRequestCo20.hpp）
 ******************************************************************************/
#include "StepHttpRequestCo20.hpp"
#include "util/CommonUtils.hpp"

namespace core
{

net::Task<> StepHttpRequestCo20::CoroutineMain()
{
    LOG4_TRACE("%s() start", __FUNCTION__);

    try
    {
        LOG4_TRACE("%s() request baidu, testVal:%u", __FUNCTION__, ++m_uiTestVal);
        const bool bSuccess = co_await HttpGetAsync("http://www.baidu.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.baidu.com/ error");
            Response(1);
            co_return;
        }

        LOG4_TRACE("%s() complete, testVal:%u", __FUNCTION__, ++m_uiTestVal);
        Response(0);
    }
    catch (const std::exception& e)
    {
        LOG4_ERROR("%s() exception: %s", __FUNCTION__, e.what());
        Response(1);
    }
    catch (...)
    {
        LOG4_ERROR("%s() unknown exception", __FUNCTION__);
        Response(1);
    }

    co_return;
}

void StepHttpRequestCo20::Response(int nCode)
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
    oHttpMsg.set_body(oJsonObj.ToString());
    GetLabor()->SendTo(m_stReqMsgShell, oHttpMsg);
}

} // namespace core
