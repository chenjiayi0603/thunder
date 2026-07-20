/*******************************************************************************
 * Project:  LogicServer
 * @file     CmdRobotPreQuestion.cpp
 * @brief
 * @author   cjy
 * @date:    2016年12月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdGetToken.hpp"
#include "LogicSession.h"

MUDULE_CREATE(robot::CmdGetToken);

namespace robot
{

CmdGetToken::CmdGetToken() {}

CmdGetToken::~CmdGetToken() {}

bool CmdGetToken::Init()
{
    if (!GetLogicSession())
    {
        LOG4_ERROR("failed to get GetLogicSession");
        return false;
    }
    return true;
}

bool CmdGetToken::AnyMessage(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    LOG4_TRACE("%s() %s", __FUNCTION__, oInMsgBody.DebugString().c_str());
    util::CJsonObject oJson;
    if (!oJson.Parse(oInMsgBody.body()))
    {
        LOG4_ERROR("%s()", __FUNCTION__);
        Response(stMsgShell, oInMsgHead, 1);
        return false;
    }

    std::string strToken   = oJson("token");
    std::string strKey     = oJson("key");
    std::string strAddress = oJson("address");
    if (strAddress.empty() || strToken.empty() || strKey.empty())
    {
        LOG4_ERROR("%s() strAddress.empty() || strToken.empty() || strKey.empty()", __FUNCTION__);
        Response(stMsgShell, oInMsgHead, 1);
        return false;
    }
    LogicSession* pSession = GetLogicSession();
    if (!pSession)
    {
        LOG4_ERROR("%s() GetLogicSession failed", __FUNCTION__);
        Response(stMsgShell, oInMsgHead, 1);
        return false;
    }
    // genkey=1 → 存储 token（Interface/HelloHttp GenKey 全链路）
    if (oJson("genkey") == "1")
    {
        LOG4_TRACE("%s() genkey store token (strToken:%s,strKey:%s)", __FUNCTION__,
                   strToken.c_str(), strKey.c_str());
        pSession->GenToken(strToken, strKey);
        Response(stMsgShell, oInMsgHead, 0);
        return true;
    }
    // verifykey=1 → 验证 token key 排列（Interface/HelloHttp VerifyKey）
    else if (oJson("verifykey") == "1")
    {
        LOG4_TRACE("%s() verifykey check (strToken:%s,strKey:%s)", __FUNCTION__,
                   strToken.c_str(), strKey.c_str());
        bool ret = pSession->VerifyTokenPermutation(strToken, strKey);
        LOG4_INFO("%s() VerifyTokenPermutation(%s,%s) ret(%d)", __FUNCTION__,
                  strToken.c_str(), strKey.c_str(), ret);
        if (!ret)
        {
            LOG4_INFO("%s() VerifyTokenPermutation(%s,%s) failed", __FUNCTION__,
                      strToken.c_str(), strKey.c_str());
        }
        Response(stMsgShell, oInMsgHead, ret ? 0 : 1);
        return true;
    }
    else
    {
        LOG4_ERROR("%s() error param (need genkey=1 or verifykey=1)", __FUNCTION__);
        Response(stMsgShell, oInMsgHead, 1);
        return false;
    }
    return true;
}

void CmdGetToken::Response(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, int code)
{
    util::CJsonObject oRsp;
    oRsp.Add("code", code);
    GetLabor()->SendToClient(stMsgShell, oInMsgHead, oRsp.ToString());
}

} /* namespace robot */
