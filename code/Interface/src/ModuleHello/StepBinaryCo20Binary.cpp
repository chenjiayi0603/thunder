/*******************************************************************************
 * Interface 节点独立副本（见 StepBinaryCo20Binary.hpp）
 ******************************************************************************/
#include "StepBinaryCo20Binary.hpp"
#include "util/CommonUtils.hpp"

namespace core
{

namespace
{
constexpr uint32_t kCmdToLogicTokenBinaryDemo = 10001u;
constexpr size_t kMaxPassthroughBytes = 64 * 1024;
}

StepBinaryCo20Binary::StepBinaryCo20Binary(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
    : net::StepCo20(stMsgShell, oInHttpMsg)
{
    m_strStepDesc = "StepBinaryCo20Binary";
}

void StepBinaryCo20Binary::OnCoroutineComplete(bool /*bSuccess*/) {}

net::Task<> StepBinaryCo20Binary::CoroutineMain()
{
    LOG4_TRACE("%s() cmd %u seq %u", __FUNCTION__, m_oReqMsgHead.cmd(), m_oReqMsgHead.seq());

    util::CJsonObject obj;
    if (!obj.Parse(m_oInHttpMsg.body()))
    {
        ResponseToClient(400, R"({"code":1,"msg":"invalid json body"})");
        co_return;
    }
    std::string strOption;
    if (!obj.Get("option", strOption) || strOption.empty())
    {
        ResponseToClient(400, R"({"code":1,"msg":"missing option"})");
        co_return;
    }

    std::string strPassthrough;
    bool forwardRawLogicBody = false;

    if (strOption == "GenKey")
    {
        const std::string address = GetLabor()->GetClientAddr(GetReqMsgShell());
        util::CJsonObject oJson;
        oJson.Add("token", std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(), GetLabor()->GetWorkerIndex())));
        oJson.Add("key", std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(), GetLabor()->GetWorkerIndex())));
        oJson.Add("genkey", "1");
        oJson.Add("address", address);
        strPassthrough = oJson.ToString();
        forwardRawLogicBody = true;
    }
    else if (strOption == "VerifyKey")
    {
        std::string strToken;
        std::string strKey;
        obj.Get("token", strToken);
        obj.Get("key", strKey);
        if (strToken.empty() || strKey.empty())
        {
            ResponseToClient(400, R"({"code":1,"msg":"token or key empty"})");
            co_return;
        }
        util::CJsonObject oJson;
        oJson.Add("token", strToken);
        oJson.Add("key", strKey);
        oJson.Add("verifykey", "1");
        oJson.Add("address", GetLabor()->GetClientAddr(GetReqMsgShell()));
        strPassthrough = oJson.ToString();
        forwardRawLogicBody = true;
    }
    else if (strOption == "TestStepCo20Binary")
    {
        strPassthrough = m_oInHttpMsg.body();
        if (strPassthrough.size() > kMaxPassthroughBytes)
        {
            strPassthrough.resize(kMaxPassthroughBytes);
        }
        forwardRawLogicBody = false;
    }
    else
    {
        ResponseToClient(400, R"({"code":1,"msg":"option not handled by StepBinaryCo20Binary"})");
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

    const bool okLogic = co_await SendToInternalByNodeTypeAsync("LOGIC", oOutHead, oOutBody);

    MsgBody logicBodySnap;
    if (okLogic)
    {
        logicBodySnap = GetLastRspMsgBody();
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
        ResponseToClient(httpCode, logicBody);
        co_return;
    }

    util::CJsonObject oRsp;
    oRsp.Add("code", okLogic ? 0 : 1);
    oRsp.Add("msg", okLogic ? "ok" : "logic step failed");
    oRsp.Add("demo", "StepBinaryCo20Binary: co_await SendToInternalByNodeTypeAsync(LOGIC)");
    oRsp.Add("ok_logic", okLogic);
    oRsp.Add("req_cmd", static_cast<int32_t>(m_oReqMsgHead.cmd()));
    oRsp.Add("req_seq", static_cast<int32_t>(m_oReqMsgHead.seq()));
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

} // namespace core
