/*******************************************************************************
 * Project:  LogicServer
 * @file     CmdRobotPreQuestion.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年12月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "util/CommonUtils.hpp"
#include "CmdGetToken.hpp"
#include "LogicSession.h"

MUDULE_CREATE(robot::CmdGetToken);

namespace robot
{

CmdGetToken::CmdGetToken()
{
}

CmdGetToken::~CmdGetToken()
{
}

bool CmdGetToken::Init()
{
	if(!GetLogicSession())
	{
		LOG4_ERROR("failed to get GetLogicSession");
		return false;
	}
	return true;
}

bool CmdGetToken::AnyMessage(
                const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    LOG4_TRACE("%s() %s", __FUNCTION__,oInMsgHead.DebugString().c_str());
    util::CJsonObject oJson;
    if (!oJson.Parse(oInMsgBody.body()))
    {
    	LOG4_ERROR("%s()", __FUNCTION__);
    	Response(stMsgShell,oInMsgHead,1);
    	return false;
    }

    std::string strToken = oJson("token");
    std::string strKey = oJson("key");

    std::string strAddress = oJson("address");
    if (strAddress.empty() || strToken.empty() || strKey.empty())
    {
    	LOG4_ERROR("%s() strAddress.empty() || strToken.empty() || strKey.empty()", __FUNCTION__);
		Response(stMsgShell,oInMsgHead,1);
		return false;
    }
    Token token = g_pLogicSession->GetToken(strAddress,strToken,strKey);
    LOG4_INFO("%s() strAddress(%s),token strID(%s) strToken(%s),strKey(%s)",
        		__FUNCTION__,strAddress.c_str(),token.strID.c_str(),
        		token.strToken.c_str(),token.strKey.c_str());
    util::CJsonObject oRsp;
	oRsp.Add("code", 0);
	oRsp.Add("msg", "ok");
	oRsp.Add("token",token.strToken);
	oRsp.Add("time_create",token.m_uiTimeCreate);
	oRsp.Add("time_out",token.m_uiTimeOut);
	LOG4_INFO("%s() oRsp:%s",__FUNCTION__,oRsp.ToString().c_str());
	return net::SendToClient(stMsgShell,oInMsgHead,oRsp.ToString());
}

void CmdGetToken::Response(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,int code)
{
    util::CJsonObject oRsp;
    oRsp.Add("code", code);
    oRsp.Add("msg", "ok");
    net::SendToClient(stMsgShell,oInMsgHead,oRsp.ToString());
}


} /* namespace robot */
