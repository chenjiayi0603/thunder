#include "StepHttpRequestCo.hpp"
#include "util/CommonUtils.hpp"

namespace core
{

net::Task<> StepHttpRequestCo::CoroutineMain()
{
    LOG4_TRACE("%s() start", __FUNCTION__);

    try
    {
        // 状态0: 请求百度
        LOG4_TRACE("%s() state 0: request baidu, testVal:%u", __FUNCTION__, ++m_uiTestVal);
        bool bSuccess = co_await HttpGetAsync("http://www.baidu.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.baidu.com/ error");
            Response(1);
            co_return;
        }

        // 状态1: 请求搜狗
        LOG4_TRACE("%s() state 1: request sogou, testVal:%u", __FUNCTION__, ++m_uiTestVal);
        bSuccess = co_await HttpGetAsync("http://www.sogou.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.sogou.com/ error");
            Response(1);
            co_return;
        }

        // 状态2: 请求支付宝
        LOG4_TRACE("%s() state 2: request alipay, testVal:%u", __FUNCTION__, ++m_uiTestVal);
        bSuccess = co_await HttpGetAsync("http://www.alipay.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.alipay.com/ error");
            Response(1);
            co_return;
        }

        // 状态3: 完成
        LOG4_TRACE("%s() state 3: complete, testVal:%u", __FUNCTION__, ++m_uiTestVal);
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

void StepHttpRequestCo::Response(int nCode)
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
