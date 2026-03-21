/*******************************************************************************
 * Project:  Interface
 * @file     ModuleInterface.cpp
 * @brief    gentoken + HTTP/二进制 JSON option 协程演示（协程源在 Interface 目录内，独立于 Hello）
 ******************************************************************************/
#include "ModuleInterface.hpp"
#include "StepBinaryCo20Binary.hpp"
#include "StepHttpRequestCo20.hpp"
#include "util/StringCoder.hpp"
#include "util/CommonUtils.hpp"
#include <algorithm>
#include <map>
#include <vector>

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

    /// StepCo20：HTTP 用 HttpMsg 构造 Step（否则 ResponseToClient 误走二进制回包，curl 会 Empty reply）
    if ("TestStepCo20Binary" == strOption)
    {
        LOG4_TRACE("%s TestStepCo20Binary", __FUNCTION__);
        return GetLabor()->ExecStep(new core::StepBinaryCo20Binary(stMsgShell, oInHttpMsg), 0.0);
    }

    LOG4_TRACE("%s unknown option %s", __FUNCTION__, strOption.c_str());
    replyOk(0);
    return true;
}

void ModuleHello::GenKey(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    std::string strToken = std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(), GetLabor()->GetWorkerIndex()));
    std::string strKey   = std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(), GetLabor()->GetWorkerIndex()));

    {
        auto callback = [](const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, net::StepParam* data, net::Step* pStep) {
            LOG4_TRACE("callback %s", oInMsgBody.body().c_str());
            {
                util::CJsonObject oJson;
                oJson.Parse(oInMsgBody.body());
                int code(1);
                oJson.Get("code", code);
                pStep->SendToClient(oInMsgBody.body(), code == 0 ? 200 : 400);
            }
        };
        util::CJsonObject oJson;
        std::string       address = GetLabor()->GetClientAddr(stMsgShell);
        oJson.Add("token", strToken);
        oJson.Add("key", strKey);
        oJson.Add("genkey", "1");

        oJson.Add("address", address);
        LOG4_DEBUG("oJson(%s)", oJson.ToString().c_str());
        GetLabor()->SendToCallback(new net::DataStep(stMsgShell, oInHttpMsg), GET_TOKEN_GEN, oJson.ToString(), callback, "LOGIC", address);
    }
}

void ModuleHello::VerifyKey(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    std::map<std::string, std::string> mapParameters;
    util::DecodeParameter(oInHttpMsg.url(), mapParameters, '?');
    util::CJsonObject oJson;
    for (auto p : mapParameters)
    {
        oJson.Add(p.first, p.second);
        LOG4_DEBUG("Param(%s %s)", p.first.c_str(), p.second.c_str());
    }
    std::string strToken = oJson("token");
    std::string strKey   = oJson("key");
    if (strToken.empty() || strKey.empty())
    {
        LOG4_ERROR("%s() strToken.empty() || strKey.empty()", __FUNCTION__);
        GetLabor()->SendToClient(stMsgShell, oInHttpMsg, "strToken empty or strKey empty", 400);
        return;
    }
    auto callback = [](const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, net::StepParam* data, net::Step* pStep) {
        LOG4_TRACE("callback %s", oInMsgBody.body().c_str());
        util::CJsonObject oJson;
        oJson.Parse(oInMsgBody.body());
        int code(1);
        oJson.Get("code", code);
        pStep->SendToClient(oInMsgBody.body(), code == 0 ? 200 : 400);
    };

    oJson.Add("verifykey", "1");
    std::string address = GetLabor()->GetClientAddr(stMsgShell);
    oJson.Add("address", address);
    LOG4_DEBUG("oJson(%s)", oJson.ToString().c_str());
    GetLabor()->SendToCallback(new net::DataStep(stMsgShell, oInHttpMsg), GET_TOKEN_GEN, oJson.ToString(), callback, "LOGIC", address);
}

bool ModuleHello::AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    LOG4_DEBUG("oInHttpMsg:%s", oInHttpMsg.DebugString().c_str());

    if (DispatchJsonTestsFromBody(stMsgShell, oInHttpMsg))
    {
        return true;
    }

    auto iter = std::find(oInHttpMsg.url().begin(), oInHttpMsg.url().end(), '?');
    if (iter == oInHttpMsg.url().end())
    {
        GenKey(stMsgShell, oInHttpMsg);
    }
    else
    {
        VerifyKey(stMsgShell, oInHttpMsg);
    }
    return true;
}

bool ModuleHello::AnyMessage(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    LOG4_TRACE("%s cmd %u seq %u", __FUNCTION__, oInMsgHead.cmd(), oInMsgHead.seq());
    HttpMsg oHttp = MakeSyntheticHttpFromJsonBody(oInMsgBody.body());
    if (DispatchJsonTestsFromBody(stMsgShell, oHttp))
    {
        return true;
    }
    return false;
}

void ModuleHello::Response(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg, int iCode)
{
    util::CJsonObject oRsp;
    oRsp.Add("code", iCode);
    oRsp.Add("msg", "ok");
    GetLabor()->SendToClient(stMsgShell, oInHttpMsg, oRsp.ToString());
}

} // namespace robot
