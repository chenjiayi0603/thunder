#include "HttpStateFuncCo.hpp"

namespace core
{

HttpStateFuncCo::HttpStateFuncCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
    : net::CoroutineState(stMsgShell, oInHttpMsg)
{
    m_strStepDesc = "HttpStateFuncCo";
    SetData(new HttpStateFuncParam());
}

HttpStateFuncCo::~HttpStateFuncCo()
{
    // SetData 由业务层分配，与旧 StepState 示例一致，不在此 delete
}

net::CoTask<void> HttpStateFuncCo::Run()
{
    auto* param = dynamic_cast<HttpStateFuncParam*>(GetData());
    if (!param)
    {
        LOG4_ERROR("%s() GetData failed", __FUNCTION__);
        SendFail();
        co_return;
    }

    const std::string kUrl("http://www.baidu.com/");

    // State0
    LOG4_TRACE("%s GetLastState:%u GetCurrentState(%u) val(%u) m(%u)", __FUNCTION__,
               0u, 0u, param->Inc(), static_cast<unsigned>(param->m.size()));
    if (!co_await HttpGetAsync(kUrl))
    {
        SendFail();
        co_return;
    }

    // State1（原 SetNextState(3) 跳过 State2）
    LOG4_TRACE("%s GetLastState:%u GetCurrentState(%u) val(%u) m(%u)", __FUNCTION__,
               0u, 1u, param->Inc(), static_cast<unsigned>(param->m.size()));
    if (!co_await HttpGetAsync(kUrl))
    {
        SendFail();
        co_return;
    }

    // State3（State2 被跳过）
    LOG4_TRACE("%s GetLastState:%u GetCurrentState(%u) val(%u) m(%u)", __FUNCTION__,
               1u, 3u, param->Inc(), static_cast<unsigned>(param->m.size()));
    if (!co_await HttpGetAsync(kUrl))
    {
        SendFail();
        co_return;
    }

    SendSucc();
    co_return;
}

void HttpStateFuncCo::SendSucc()
{
    if (auto* param = dynamic_cast<HttpStateFuncParam*>(GetData()))
    {
        LOG4_TRACE("%s GetLastState:%u GetCurrentState(%u) val(%u) m(%u)", __FUNCTION__,
                   3u, 4u, param->Inc(), static_cast<unsigned>(param->m.size()));
    }
    util::CJsonObject oJsonObj;
    oJsonObj.Add("code", 0);
    oJsonObj.Add("msg", "ok");
    SendToClient(oJsonObj.ToString());
}

void HttpStateFuncCo::SendFail()
{
    if (auto* param = dynamic_cast<HttpStateFuncParam*>(GetData()))
    {
        LOG4_TRACE("%s GetLastState:%u GetCurrentState(%u) val(%u) m(%u)", __FUNCTION__,
                   0u, 0u, param->Inc(), static_cast<unsigned>(param->m.size()));
    }
    util::CJsonObject oJsonObj;
    oJsonObj.Add("code", 1);
    oJsonObj.Add("msg", "fail");
    SendToClient(oJsonObj.ToString());
}

} // namespace core
