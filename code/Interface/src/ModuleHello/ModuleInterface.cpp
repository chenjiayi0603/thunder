/*******************************************************************************
 * Project:  Interface
 * @file     ModuleInterface.cpp
 * @brief    gentoken + HTTP/二进制 JSON option 协程演示（协程源在 Interface 目录内，独立于 Hello）
 ******************************************************************************/
#include "ModuleInterface.hpp"
#include "coro/StepCo20Func.hpp"
#include "util/CommonUtils.hpp"

#include <memory>

MUDULE_CREATE(robot::ModuleHello);

namespace robot
{

namespace {

constexpr uint32_t kCmdToLogicTokenBinaryDemo = 10001u;
constexpr size_t kMaxPassthroughBytes = 64 * 1024;
constexpr size_t kMaxUpstreamBodyInClientJson = static_cast<size_t>(64 * 1024);

/** TestStepHttpRequestCo：原 StepHttpRequestCo::Response，用 StepCo20Func 内联时复用 */
void SendTestStepHttpRequestCoJsonResponse(net::StepCo20& step, int nCode,
                                          const HttpMsg* pUpstreamRsp, uint32_t uiTestVal)
{
    HttpMsg oHttpMsg;
    util::CJsonObject oJsonObj;
    oHttpMsg.set_type(HTTP_RESPONSE);
    oHttpMsg.set_status_code(200);
    oHttpMsg.set_http_major(step.m_oInHttpMsg.http_major());
    oHttpMsg.set_http_minor(step.m_oInHttpMsg.http_minor());
    oJsonObj.Add("code", nCode);
    oJsonObj.Add("msg", "ok");
    oJsonObj.Add("testVal", uiTestVal);
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
    GetLabor()->SendTo(step.m_stReqMsgShell, oHttpMsg);
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

    if ("TestStepHttpRequestCo" == strOption)
    {
        LOG4_TRACE("%s TestStepHttpRequestCo (StepCo20Func)", __FUNCTION__);
        HttpMsg oHttp = MakeSyntheticHttpFromJsonBody(body);
        auto pTestVal = std::make_shared<uint32_t>(0);
        return net::Launch(new net::StepCo20Func(stMsgShell, oHttp,
            [pTestVal](net::StepCo20& step) -> net::Task<> {
                LOG4_TRACE("TestStepHttpRequestCo lambda start");
                try
                {
                    LOG4_TRACE("TestStepHttpRequestCo request example.com, testVal:%u", ++*pTestVal);
                    const bool bSuccess = co_await step.HttpGetAsync("http://example.com/");
                    if (!bSuccess)
                    {
                        LOG4_ERROR("HttpGet http://example.com/ error");
                        SendTestStepHttpRequestCoJsonResponse(step, 1, nullptr, *pTestVal);
                        co_return;
                    }

                    LOG4_TRACE("TestStepHttpRequestCo complete, testVal:%u", ++*pTestVal);
                    SendTestStepHttpRequestCoJsonResponse(step, 0, &step.GetLastRspHttpMsg(), *pTestVal);
                }
                catch (const std::exception& e)
                {
                    LOG4_ERROR("TestStepHttpRequestCo exception: %s", e.what());
                    SendTestStepHttpRequestCoJsonResponse(step, 1, nullptr, *pTestVal);
                }
                catch (...)
                {
                    LOG4_ERROR("TestStepHttpRequestCo unknown exception");
                    SendTestStepHttpRequestCoJsonResponse(step, 1, nullptr, *pTestVal);
                }
                co_return;
            }));
    }

    // GenKey / VerifyKey （协程 co_await→LOGIC，StepCo20Func + lambda）
    if ("GenKey" == strOption || "VerifyKey" == strOption)
    {
        LOG4_TRACE("%s StepCo20Func GenKey/VerifyKey option=%s", __FUNCTION__, strOption.c_str());
        return net::Launch(new net::StepCo20Func(stMsgShell, oInHttpMsg,
            [](net::StepCo20& step) -> net::Task<> {
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

                if (opt == "GenKey")
                {
                    const std::string address = GetLabor()->GetClientAddr(step.GetReqMsgShell());
                    util::CJsonObject oJson;
                    oJson.Add("token",
                              std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(),
                                                             GetLabor()->GetWorkerIndex())));
                    oJson.Add("key",
                              std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(),
                                                             GetLabor()->GetWorkerIndex())));
                    oJson.Add("genkey", "1");
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
                    int httpCode = 400;
                    if (oJson.Parse(logicBody))
                    {
                        int code = 1;
                        if (oJson.Get("code", code) && code == 0)
                        {
                            httpCode = 200;
                        }
                    }
                    step.ResponseToClient(httpCode, logicBody);
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
            }));
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
