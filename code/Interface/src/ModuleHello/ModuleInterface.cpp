/*******************************************************************************
 * Project:  Hello
 * @file     ModuleHello.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年2月1日
 * @note
 * Modify history:
 ******************************************************************************/
#include <map>
#include "util/UnixTime.hpp"
#include "util/HashCalc.hpp"
#include "util/StringCoder.hpp"
#include "ModuleInterface.hpp"

MUDULE_CREATE(robot::ModuleHello);

namespace robot
{

ModuleHello::ModuleHello()
{
}

ModuleHello::~ModuleHello()
{
}

bool ModuleHello::Init()
{
    return(true);
}



bool ModuleHello::AnyMessage(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	LOG4_DEBUG("body %s oInHttpMsg %s url:%s",oInHttpMsg.body().c_str(),oInHttpMsg.DebugString().c_str(),oInHttpMsg.url().c_str());
	std::map<std::string, std::string> mapParameters;
	util::DecodeParameter(oInHttpMsg.url(),mapParameters,'?');
	util::CJsonObject oJson;
	for(auto p:mapParameters)
	{
		oJson.Add(p.first,p.second);
	}
	auto callback = [] (const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,void* data,net::Step*pStep)
	{
		LOG4_TRACE("callback %s",oInMsgBody.body().c_str());
		util::CJsonObject oRsp;
		oRsp.Add("code", 0);
		oRsp.Add("msg", "ok");
		oRsp.Add("data",oInMsgBody.body());
		pStep->SendToClient(oRsp.ToString());
	};

	std::string address = g_pLabor->GetClientAddr(stMsgShell);
	oJson.Add("address",address);
	LOG4_DEBUG("oJson(%s)",oJson.ToString().c_str());
	int64 mod = util::CalcKeyHash(address.c_str(),address.size());
	return net::SendToModCallback(new net::DataStep(stMsgShell,oInHttpMsg),GET_TOKEN_GEN,oJson.ToString(),callback,mod,"LOGIC");
}


void ModuleHello::Response(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,int iCode)
{
    util::CJsonObject oRsp;
    oRsp.Add("code", iCode);
    oRsp.Add("msg", "ok");
    net::SendToClient(stMsgShell,oInHttpMsg,oRsp.ToString());
}

} /* namespace core */
