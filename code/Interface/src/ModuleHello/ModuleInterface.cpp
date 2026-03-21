/*******************************************************************************
 * Project:  Interface
 * @file     ModuleInterface.cpp
 * @brief    gentoken + HTTP/二进制 JSON option 协程演示（协程源在 Interface 目录内，独立于 Hello）
 ******************************************************************************/
#include "ModuleInterface.hpp"
#include "HttpRequestCo.hpp"
#include "StepHttpRequestCo20.hpp"
#include "step/StepCo20.hpp"
#include "util/StringCoder.hpp"
#include "util/CommonUtils.hpp"
#include <algorithm>
#include <map>
#include <vector>

MUDULE_CREATE(robot::ModuleHello);

namespace robot
{

namespace {

constexpr uint32_t kCmdToLogicTokenBinaryDemo = 10001u;

/**
 * TestStepCo20Binary：co_await SendToInternalByNodeTypeAsync("LOGIC",...)；HTTP 入口须用 HttpMsg 构造以正确回 HTTP。
 * 发往 LOGIC 的 MsgBody 为客户端消息体原样透传（不再包 option/via/forward），便于 Cmd 侧按 JSON 字段解析。
 */
class StepBinaryCo20Binary : public net::StepCo20
{
public:
    /// 须用 HttpMsg 构造，保证 ResponseToClient 走 HTTP 分支（m_oReqMsgHead.cmd()==0）
    StepBinaryCo20Binary(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    net::Task<> CoroutineMain() override;

protected:
    void OnCoroutineComplete(bool bSuccess) override;
};

StepBinaryCo20Binary::StepBinaryCo20Binary(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
    : net::StepCo20(stMsgShell, oInHttpMsg)
{
    m_strStepDesc = "StepBinaryCo20Binary";
}

void StepBinaryCo20Binary::OnCoroutineComplete(bool /*bSuccess*/) {}

net::Task<> StepBinaryCo20Binary::CoroutineMain()
{
    LOG4_TRACE("%s() cmd %u seq %u", __FUNCTION__, m_oReqMsgHead.cmd(), m_oReqMsgHead.seq());

    // 透传消息体：与 DispatchJsonTestsFromBody 传入的 HttpMsg.body() 一致
    std::string strPassthrough = m_oInHttpMsg.body();
    constexpr size_t kMaxPassthroughBytes = 64 * 1024;
    if (strPassthrough.size() > kMaxPassthroughBytes)
    {
        strPassthrough.resize(kMaxPassthroughBytes);
    }

    MsgBody oOutBody;
    oOutBody.set_body(std::move(strPassthrough));
    MsgHead oOutHead;
    oOutHead.set_cmd(kCmdToLogicTokenBinaryDemo);

    const bool okLogic = co_await SendToInternalByNodeTypeAsync("LOGIC", oOutHead, oOutBody);

    MsgHead logicHeadSnap;
    MsgBody logicBodySnap;
    if (okLogic)
    {
        logicHeadSnap = GetLastRspMsgHead();
        logicBodySnap = GetLastRspMsgBody();
    }

    util::CJsonObject oRsp;
    oRsp.Add("code", okLogic ? 0 : 1);
    oRsp.Add("msg", okLogic ? "ok" : "logic step failed");
    oRsp.Add("demo", "ModuleInterface.cpp: co_await SendToInternalByNodeTypeAsync(LOGIC)");
    oRsp.Add("ok_logic", okLogic);
    oRsp.Add("req_cmd", static_cast<int32_t>(m_oReqMsgHead.cmd()));
    oRsp.Add("req_seq", static_cast<int32_t>(m_oReqMsgHead.seq()));
    // SendToInternalByNodeTypeAsync 为 false：多为 Worker::SendToNext 找不到 LOGIC（NodesMgr 无节点），
    // 即 Center 未起、Logic 未注册、或 Interface 与 Center 地址不一致；见 Interface 日志 no tagMsgShell match LOGIC
    if (!okLogic)
    {
        oRsp.Add(
            "hint",
            "SendToSession(LOGIC) failed: no LOGIC node in Interface route table (NodesMgr). "
            "Check Center is listening, Logic registered to Center, Interface.json center matches Center; "
            "start order Center -> Logic -> Interface. See Interface log: no tagMsgShell match LOGIC");
    }

    if (okLogic)
    {
        const std::string& logicBody = logicBodySnap.body();
        std::string preview = logicBody.size() > 1024 ? logicBody.substr(0, 1024) + "..." : logicBody;
        oRsp.Add("logic_rsp_preview", preview);
    }
    ResponseToClient(200, oRsp.ToString());
    co_return;
}

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
    if ("TestHttpRequestCo" == strOption)
    {
        LOG4_TRACE("%s TestHttpRequestCo", __FUNCTION__);
        HttpMsg oHttp = MakeSyntheticHttpFromJsonBody(body);
        return net::Launch(new core::HttpRequestCo(stMsgShell, oHttp));
    }

    /// StepCo20：HTTP 用 HttpMsg 构造 Step（否则 ResponseToClient 误走二进制回包，curl 会 Empty reply）
    if ("TestStepCo20Binary" == strOption)
    {
        LOG4_TRACE("%s TestStepCo20Binary", __FUNCTION__);
        return GetLabor()->ExecStep(new StepBinaryCo20Binary(stMsgShell, oInHttpMsg), 0.0);
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
