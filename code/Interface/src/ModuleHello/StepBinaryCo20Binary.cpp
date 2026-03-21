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
    oRsp.Add("demo", "StepBinaryCo20Binary: co_await SendToInternalByNodeTypeAsync(LOGIC)");
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

} // namespace core
