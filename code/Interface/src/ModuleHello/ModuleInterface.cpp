/*******************************************************************************
 * Project:  Interface
 * @file     ModuleInterface.cpp
 * @brief    gentoken + HTTP/二进制 JSON option 协程演示（协程源在 Interface 目录内，独立于 Hello）
 ******************************************************************************/
#include "ModuleInterface.hpp"
#include "StepBinaryCo20Binary.hpp"
#include "StepHttpRequestCo20.hpp"
#include "util/CommonUtils.hpp"

MUDULE_CREATE(robot::ModuleHello);

namespace robot
{

namespace {

HttpMsg MakeSyntheticHttpFromJsonBody(const std::string& body)
{
    HttpMsg oHttp;
    oHttp.set_body(body);
    oHttp.set_type(HTTP_REQUEST);
    oHttp.set_method(HTTP_POST);
    oHttp.set_url("http://127.0.0.1/interface");
    oHttp.set_http_major(1);
    oHttp.set_http_minor(1);
    return oHttp;
}

} // namespace

ModuleHello::ModuleHello() {}

ModuleHello::~ModuleHello() {}

bool ModuleHello::Init()
{
    return true;
}

bool ModuleHello::DispatchJsonTestsFromBody(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    const std::string& body = oInHttpMsg.body();
    if (body.empty())
    {
        return false;
    }
    util::CJsonObject obj;
    if (!obj.Parse(body))
    {
        return false;
    }
    std::string strOption;
    if (!obj.Get("option", strOption) || strOption.empty())
    {
        return false;
    }

    auto replyOk = [&](int code) { Response(stMsgShell, oInHttpMsg, code); };

    if ("Echo" == strOption)
    {
        replyOk(0);
        return true;
    }

    if ("TestHttpRequestCo20" == strOption)
    {
        LOG4_TRACE("%s TestHttpRequestCo20", __FUNCTION__);
        HttpMsg oHttp = MakeSyntheticHttpFromJsonBody(body);
        return net::Launch(new core::StepHttpRequestCo20(stMsgShell, oHttp));
    }

    // GenKey / VerifyKey （协程 co_await→LOGIC）
    if ("GenKey" == strOption || "VerifyKey" == strOption)
    {
        LOG4_TRACE("%s StepBinaryCo20Binary option=%s", __FUNCTION__, strOption.c_str());
        return net::Launch(new core::StepBinaryCo20Binary(stMsgShell, oInHttpMsg));
    }

    LOG4_TRACE("%s unknown option %s", __FUNCTION__, strOption.c_str());
    replyOk(0);
    return true;
}

bool ModuleHello::AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    LOG4_DEBUG("oInHttpMsg:%s", oInHttpMsg.DebugString().c_str());
    return DispatchJsonTestsFromBody(stMsgShell, oInHttpMsg);
}

void ModuleHello::Response(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg, int iCode)
{
    util::CJsonObject oRsp;
    oRsp.Add("code", iCode);
    oRsp.Add("msg", "ok");
    GetLabor()->SendToClient(stMsgShell, oInHttpMsg, oRsp.ToString());
}

} // namespace robot
