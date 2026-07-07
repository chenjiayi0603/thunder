#include "HttpRequestCo.hpp"
#include "util/CommonUtils.hpp"

namespace core
{

net::AsyncTask HttpRequestCo::StepAsync()
{
    LOG4_TRACE("%s() start", __FUNCTION__);
    return AsyncBody(*this);
}

net::AsyncTask HttpRequestCo::AsyncBody(net::StepCo20& st)
{
    auto& self = static_cast<HttpRequestCo&>(st);

    try
    {
        // ===== 改造前：需要 5 个 State 函数 =====
        // State0() { HttpGet("http://baidu.com"); return RUNNING; }
        // State1() { HttpGet("http://sogou.com"); SetNextState(4); return RUNNING; }
        // State2() { /* 其他逻辑 */ return RUNNING; }
        // State3() { /* 其他逻辑 */ return RUNNING; }
        // State4() { Response(0); return COMPLETED; }

        // ===== 改造后：1 个线性协程函数（协程形参 StepCo20& → promise_type(StepCo20&)）=====

        // 状态0: 请求百度
        LOG4_TRACE("HttpRequestCo::StepAsync state 0: request baidu, testVal:%u", ++self.m_uiTestVal);
        bool bSuccess = co_await self.HttpGetAsync("http://www.baidu.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.baidu.com/ error");
            self.Response(1);
            co_return;
        }

        // 状态1: 请求搜狗
        LOG4_TRACE("HttpRequestCo::StepAsync state 1: request sogou, testVal:%u", ++self.m_uiTestVal);
        bSuccess = co_await self.HttpGetAsync("http://www.sogou.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.sogou.com/ error");
            self.Response(1);
            co_return;
        }

        // 状态2: 请求支付宝
        LOG4_TRACE("HttpRequestCo::StepAsync state 2: request alipay, testVal:%u", ++self.m_uiTestVal);
        bSuccess = co_await self.HttpGetAsync("http://www.alipay.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.alipay.com/ error");
            self.Response(1);
            co_return;
        }

        // 状态3: 请求腾讯
        LOG4_TRACE("HttpRequestCo::StepAsync state 3: request tencent, testVal:%u", ++self.m_uiTestVal);
        bSuccess = co_await self.HttpGetAsync("http://www.qq.com/");
        if (!bSuccess)
        {
            LOG4_ERROR("HttpGet http://www.qq.com/ error");
            self.Response(1);
            co_return;
        }

        // 状态4: 完成
        LOG4_TRACE("HttpRequestCo::StepAsync state 4: complete, testVal:%u", ++self.m_uiTestVal);
        self.Response(0);
    }
    catch (const std::exception& e)
    {
        LOG4_ERROR("HttpRequestCo::StepAsync exception: %s", e.what());
        self.Response(1);
    }
    catch (...)
    {
        LOG4_ERROR("HttpRequestCo::StepAsync unknown exception");
        self.Response(1);
    }

    co_return;
}

void HttpRequestCo::Response(int nCode)
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
    oJsonObj.Add("stepType", "HttpRequestCo_StepCo20");
    oHttpMsg.set_body(oJsonObj.ToString());
    GetLabor()->SendTo(m_stReqMsgShell, oHttpMsg);
}

} // namespace core
