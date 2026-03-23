/*******************************************************************************
 * Project:  Hello
 * @file     ModuleHello.cpp
 * @brief    精简：Echo、栈协程 TestCoroutinue、C++20 协程 HTTP 演示
 ******************************************************************************/
#include <map>

#include "util/CommonUtils.hpp"
#include "util/StringCoder.hpp"
#include "ModuleHello.hpp"
#include "StepHttpRequestCo.hpp"
#include "HttpRequestCo.hpp"
#include "CustomLogger.hpp"
#include "Interface.hpp"
#include "coro/StepCo20Func.hpp"

MUDULE_CREATE(core::ModuleHello);

namespace core
{

ModuleHello::~ModuleHello()
{
	LOG4_TRACE("%s()",__FUNCTION__);
}

bool ModuleHello::Init()
{
	LOG4_TRACE("%s() ModuleHello",__FUNCTION__);
	return true;
}

bool ModuleHello::TestMsg(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	util::CJsonObject obj;
	if (!obj.Parse(oInHttpMsg.body()))
	{
		LOG4_WARN("failed to parse %s",oInHttpMsg.body().c_str());
		return false;
	}
	std::string strOption;
	obj.Get("option",strOption);
	if ("Echo" == strOption)
	{
		Response(stMsgShell,oInHttpMsg,0);
	}
	else if ("TestCoroutinue" == strOption || "TestCoroutine" == strOption)
	{
		TestCoroutinue();
		Response(stMsgShell,oInHttpMsg,0);
	}
	else if ("TestStepHttpRequestCo" == strOption)
	{
		return TestStepHttpRequestCo(stMsgShell, oInHttpMsg);
	}
	else if ("TestHttpRequestCo" == strOption)
	{
		return TestHttpRequestCo(stMsgShell,oInHttpMsg);
	}
	else
	{
		LOG4_TRACE("no things to do");
		Response(stMsgShell,oInHttpMsg,0);
	}
	return true;
}

bool ModuleHello::AnyMessage(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("body %s oInHttpMsg %s url:%s",oInHttpMsg.body().c_str(),oInHttpMsg.DebugString().c_str(),oInHttpMsg.url().c_str());
	for(auto&& p:oInHttpMsg.params())
	{
		LOG4_TRACE("p(%s,%s)",p.first.c_str(),p.second.c_str());
	}
	TestMsg(stMsgShell,oInHttpMsg);
	return true;
}

namespace
{

net::AsyncTask GenKeyNotifyLogicCo(net::StepCo20& step, std::string jsonBody, std::string address)
{
	MsgHead head;
	head.set_cmd(GET_TOKEN_GEN);
	MsgBody body;
	body.set_body(std::move(jsonBody));
	body.set_targetid(std::move(address));
	const bool ok = co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
	LOG4_TRACE("GenKey LOGIC response ok=%d body=%s", ok,
	           ok ? step.GetLastRspMsgBody().body().c_str() : "");
	co_return;
}

net::AsyncTask VerifyKeyLogicCo(net::StepCo20& step, std::string reqBody, std::string address)
{
	MsgHead head;
	head.set_cmd(GET_TOKEN_GEN);
	MsgBody body;
	body.set_body(std::move(reqBody));
	body.set_targetid(std::move(address));
	const bool ok = co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
	if (!ok)
	{
		step.ResponseToClient(500, R"({"code":1})");
		co_return;
	}
	const std::string& logicBody = step.GetLastRspMsgBody().body();
	LOG4_TRACE("VerifyKey LOGIC body %s", logicBody.c_str());
	util::CJsonObject rspJson;
	int code = 1;
	if (rspJson.Parse(logicBody))
	{
		rspJson.Get("code", code);
	}
	step.ResponseToClient(code == 0 ? 200 : 401, logicBody);
	co_return;
}

} // namespace

void ModuleHello::GenKey(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	const std::string strToken = std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(),GetLabor()->GetWorkerIndex()));
	const std::string strKey   = std::to_string(util::GetUniqueId(GetLabor()->GetNodeId(),GetLabor()->GetWorkerIndex()));

	{
		util::CJsonObject oRsp;
		oRsp.Add("token", strToken);
		oRsp.Add("key", strKey);
		GetLabor()->SendToClient(stMsgShell, oInHttpMsg, oRsp.ToString(), 200);
	}

	// 异步通知 LOGIC（fire-and-forget：客户端响应已发出，lambda 仅记录结果）
	util::CJsonObject oJson;
	const std::string address = GetLabor()->GetClientAddr(stMsgShell);
	oJson.Add("token",   strToken);
	oJson.Add("key",     strKey);
	oJson.Add("genkey",  "1");
	oJson.Add("address", address);
	LOG4_TRACE("oJson(%s)", oJson.ToString().c_str());

	net::LaunchCo(stMsgShell, oInHttpMsg,
		[oJson, address](net::StepCo20& step) -> net::AsyncTask {
			return GenKeyNotifyLogicCo(step, oJson.ToString(), address);
		});
}

void ModuleHello::VerifyKey(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	std::map<std::string, std::string> mapParameters;
	util::DecodeParameter(oInHttpMsg.url(),mapParameters,'?');
	util::CJsonObject oJson;
	for(auto p:mapParameters)
	{
		oJson.Add(p.first,p.second);
		LOG4_TRACE("Param(%s %s)",p.first.c_str(),p.second.c_str());
	}
	std::string strToken = oJson("token");
	std::string strKey = oJson("key");
	if (strToken.empty() || strKey.empty())
	{
		LOG4_ERROR("%s() strToken.empty() || strKey.empty()", __FUNCTION__);
		GetLabor()->SendToClient(stMsgShell,oInHttpMsg,"strToken empty or strKey empty",400);
		return;
	}
	oJson.Add("verifykey", "1");
	const std::string address = GetLabor()->GetClientAddr(stMsgShell);
	oJson.Add("address", address);
	LOG4_TRACE("oJson(%s)", oJson.ToString().c_str());

	const std::string reqBody = oJson.ToString();
	net::LaunchCo(stMsgShell, oInHttpMsg,
		[reqBody, address](net::StepCo20& step) -> net::AsyncTask {
			return VerifyKeyLogicCo(step, reqBody, address);
		});
}

void ModuleHello::Response(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,int iCode)
{
	util::CJsonObject oJsonObj;
	oJsonObj.Add("code", iCode);
	oJsonObj.Add("msg", "ok");
	GetLabor()->SendToClient(stMsgShell,oInHttpMsg,oJsonObj.ToString());
}

void ModuleHello::TestCoroutinue()
{
#ifdef USE_COROUTINE
	static constexpr int kTimes = 100000;
	LOG4_TRACE("TestCoroutinue");
	struct Param : public net::tagCoroutineArg
	{
		explicit Param(int a1) : m_start1(a1) {}
		int m_start1;
	};
	auto testcoroutinue = [](void *ud) {
		auto *arg = static_cast<Param *>(ud);
		for (int i = 0; i < kTimes; ++i)
		{
			LOG4_TRACE("TestCoroutinue running id(%d),arg n(%d)", GetLabor()->CoroutineRunning(), arg->m_start1 + i);
			GetLabor()->CoroutineYield();
		}
		LOG4_INFO("TestCoroutinue done id(%d),arg base(%d)", GetLabor()->CoroutineRunning(), arg->m_start1);
	};
	GetLabor()->CoroutineNewWithArg(testcoroutinue, new Param(0));
	GetLabor()->CoroutineNewWithArg(testcoroutinue, new Param(100));

	LOG4_INFO("%s Coroutine start!", __FUNCTION__);
	m_RunClock.StartClock();
	GetLabor()->CoroutineResumeWithTimes(kTimes * 2);
	m_RunClock.EndClock();
	LOG4_INFO("%s Coroutine end! use time(%lf)", __FUNCTION__, m_RunClock.LastUseTime());
#endif
}

bool ModuleHello::TestStepHttpRequestCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	return net::Launch(new StepHttpRequestCo(stMsgShell, oInHttpMsg));
}

bool ModuleHello::TestHttpRequestCo(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	return net::Launch(new HttpRequestCo(stMsgShell, oInHttpMsg));
}

} /* namespace core */
