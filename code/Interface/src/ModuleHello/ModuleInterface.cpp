/*******************************************************************************
 * Project:  Interface
 * @file     ModuleInterface.cpp
 * @brief    gentoken + HTTP/二进制 JSON option 协程演示（协程源在 Interface 目录内，独立于 Hello）
 ******************************************************************************/
#include "ModuleInterface.hpp"
#include "coro/StepCo20Func.hpp"
#include "util/CommonUtils.hpp"

MUDULE_CREATE(robot::ModuleHello);

namespace robot
{

namespace {

constexpr uint32_t kCmdToLogicTokenBinaryDemo = 10001u;
constexpr size_t kMaxPassthroughBytes = 64 * 1024;
constexpr size_t kMaxUpstreamBodyInClientJson = static_cast<size_t>(64 * 1024);

net::AsyncTask GenKeyVerifyKeyStepCo20(net::StepCo20& step)
{
    LOG4_TRACE("GenKeyVerifyKeyCo20 cmd %u seq %u", step.GetReqMsgHead().cmd(),
               step.GetReqMsgHead().seq());

    util::CJsonObject obj;
    if (!obj.Parse(step.m_oInHttpMsg.body()))
    {
        step.ResponseToClient(400, R"({"code":1,"msg":"invalid json body"})");
        co_return;
    }
    std::string opt;
    if (!obj.Get("option", opt) || opt.empty())
    {
        step.ResponseToClient(400, R"({"code":1,"msg":"missing option"})");
        co_return;
    }

    std::string strPassthrough;
    bool forwardRawLogicBody = false;
    std::string strGenToken;  // GenKey 返回给客户端的 token
    std::string strGenKey;    // GenKey 返回给客户端的 key

    if (opt == "GenKey")
    {
        std::string address = GetLabor()->GetClientAddr(step.GetReqMsgShell());
        strGenToken = std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(),
                                                       GetLabor()->GetWorkerIndex()));
        strGenKey   = std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(),
                                                       GetLabor()->GetWorkerIndex()));
        util::CJsonObject oJson;
        oJson.Add("token",   strGenToken);
        oJson.Add("key",     strGenKey);
        oJson.Add("genkey",  "1");
        oJson.Add("address", address);
        strPassthrough = oJson.ToString();
        forwardRawLogicBody = true;
    }
    else if (opt == "VerifyKey")
    {
        std::string strToken;
        std::string strKey;
        obj.Get("token", strToken);
        obj.Get("key", strKey);
        if (strToken.empty() || strKey.empty())
        {
            step.ResponseToClient(400, R"({"code":1,"msg":"token or key empty"})");
            co_return;
        }
        util::CJsonObject oJson;
        oJson.Add("token", strToken);
        oJson.Add("key", strKey);
        oJson.Add("verifykey", "1");
        oJson.Add("address", GetLabor()->GetClientAddr(step.GetReqMsgShell()));
        strPassthrough = oJson.ToString();
        forwardRawLogicBody = true;
    }
    else if (opt == "TestStepCo20Binary")
    {
        strPassthrough = step.m_oInHttpMsg.body();
        if (strPassthrough.size() > kMaxPassthroughBytes)
        {
            strPassthrough.resize(kMaxPassthroughBytes);
        }
        forwardRawLogicBody = false;
    }
    else
    {
        step.ResponseToClient(400, R"({"code":1,"msg":"option not handled by StepCo20Func"})");
        co_return;
    }

    if (strPassthrough.size() > kMaxPassthroughBytes)
    {
        strPassthrough.resize(kMaxPassthroughBytes);
    }

    MsgBody oOutBody;
    oOutBody.set_body(std::move(strPassthrough));
    MsgHead oOutHead;
    oOutHead.set_cmd(kCmdToLogicTokenBinaryDemo);
    //正常发到 LOGIC 并等待响应时，协程会停在这一步直到回包（或对应失败）；线程一般不会空转阻塞，而是去做别的事件。
    const bool okLogic = co_await step.SendToInternalByNodeTypeAsync("LOGIC", oOutHead, oOutBody);

    MsgBody logicBodySnap;
    if (okLogic)
    {
        logicBodySnap = step.GetLastRspMsgBody();
    }

    if (okLogic && forwardRawLogicBody)
    {
        const std::string& logicBody = logicBodySnap.body();
        util::CJsonObject oJson;
        if (oJson.Parse(logicBody))
        {
            int code = 1;
            if (oJson.Get("code", code) && code == 0)
            {
                // 兼容历史逻辑返回仅含 token/key 的场景，统一补齐成功语义。
                std::string msg;
                if (!oJson.Get("msg", msg) || msg.empty())
                {
                    oJson.Add("msg", "success");
                }
                // GenKey：将 token+key 注入响应，客户端可直接用于 VerifyKey
                if (!strGenToken.empty())
                {
                    oJson.Add("token", strGenToken);
                    oJson.Add("key", strGenKey);
                }
            }
            // 统一返回 200，业务错误码在 JSON body 中表达（VerifyKey 错误 token 等）
            step.ResponseToClient(200, oJson.ToString());
            co_return;
        }
        step.ResponseToClient(200, logicBody);
        co_return;
    }

    util::CJsonObject oRsp;
    oRsp.Add("code", okLogic ? 0 : 1);
    oRsp.Add("msg", okLogic ? "ok" : "logic step failed");
    oRsp.Add("demo", "StepCo20Func: co_await SendToInternalByNodeTypeAsync(LOGIC)");
    oRsp.Add("ok_logic", okLogic);
    oRsp.Add("req_cmd", static_cast<int32_t>(step.GetReqMsgHead().cmd()));
    oRsp.Add("req_seq", static_cast<int32_t>(step.GetReqMsgHead().seq()));
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
        std::string preview =
            logicBody.size() > 1024 ? logicBody.substr(0, 1024) + "..." : logicBody;
        oRsp.Add("logic_rsp_preview", preview);
    }
    step.ResponseToClient(200, oRsp.ToString());
    co_return;
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
        Response(stMsgShell, oInHttpMsg, 400);
        return false;
    }
    util::CJsonObject obj;
    if (!obj.Parse(body))
    {
        Response(stMsgShell, oInHttpMsg, 400);
        return false;
    }
    std::string strOption;
    if (!obj.Get("option", strOption) || strOption.empty())
    {
        Response(stMsgShell, oInHttpMsg, 400);
        return false;
    }

    auto replyOk = [&](int code)->void { Response(stMsgShell, oInHttpMsg, code); return; };

    if ("Echo" == strOption)
    {
        replyOk(0);
        return true;
    }

    // GenKey / VerifyKey （协程 co_await→LOGIC，StepCo20Func + lambda）
    if ("GenKey" == strOption || "VerifyKey" == strOption)
    {
        LOG4_TRACE("%s StepCo20Func GenKey/VerifyKey option=%s", __FUNCTION__, strOption.c_str());
        return net::LaunchCo(stMsgShell, oInHttpMsg,
            [](net::StepCo20& step) -> net::AsyncTask { return GenKeyVerifyKeyStepCo20(step); });
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
