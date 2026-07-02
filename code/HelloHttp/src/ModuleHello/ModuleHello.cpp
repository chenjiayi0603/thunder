/*******************************************************************************
 * Project:  Hello
 * @file     ModuleHello.cpp
 * @brief    精简：Echo、C++20 协程 HTTP 演示
 *           协程 + 线程池（设计向伪代码，例 A/例 B）见同目录 StepCo20-threadpool-examples.md
 ******************************************************************************/
#include <map>
#include <cstring>
#include <cstdlib>

#include "util/CommonUtils.hpp"
#include "codec/StringCoder.hpp"
#include "ModuleHello.hpp"
#include "HttpRequestCo.hpp"
#include "Interface.hpp"
#include "coro/StepCo20Func.hpp"
#include "dbi/Dbi.hpp"
#include "HelloCoRedis.hpp"
#include "HelloCoMysql.hpp"
#include "HelloPoolCpu.hpp"
#include "HelloPoolBlock.hpp"

MUDULE_CREATE(core::ModuleHello);

namespace core
{

namespace
{

int JsonIntOrDefault(const util::CJsonObject& o, const char* key, int defVal)
{
	int32_t v = static_cast<int32_t>(defVal);
	if (o.Get(key, v))
	{
		return static_cast<int>(v);
	}
	std::string s;
	if (o.Get(key, s) && !s.empty())
	{
		return std::atoi(s.c_str());
	}
	return defVal;
}

std::string JsonStrOrDefault(const util::CJsonObject& o, const char* key, const std::string& def)
{
	std::string s;
	if (o.Get(key, s) && !s.empty())
	{
		return s;
	}
	return def;
}

util::tagDbConnInfo MakeTagDbConn(const std::string& host,
                                  unsigned int port,
                                  const std::string& user,
                                  const std::string& pwd,
                                  const std::string& db,
                                  const std::string& charset)
{
	util::tagDbConnInfo c{};
	std::strncpy(c.m_szDbHost, host.c_str(), sizeof(c.m_szDbHost) - 1);
	std::strncpy(c.m_szDbUser, user.c_str(), sizeof(c.m_szDbUser) - 1);
	std::strncpy(c.m_szDbPwd, pwd.c_str(), sizeof(c.m_szDbPwd) - 1);
	std::strncpy(c.m_szDbName, db.c_str(), sizeof(c.m_szDbName) - 1);
	std::strncpy(c.m_szDbCharSet, charset.c_str(), sizeof(c.m_szDbCharSet) - 1);
	c.m_uiDbPort = port;
	c.uiTimeOut = 5;
	return c;
}

} // namespace

ModuleHello::~ModuleHello()
{
	LOG4_TRACE("%s()", __FUNCTION__);
}

bool ModuleHello::Init()
{
	LOG4_TRACE("%s() ModuleHello", __FUNCTION__);
	return true;
}

bool ModuleHello::TestMsg(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
	util::CJsonObject obj;
	if (!obj.Parse(oInHttpMsg.body()))
	{
		LOG4_WARN("failed to parse %s", oInHttpMsg.body().c_str());
		return false;
	}
	std::string strOption;
	obj.Get("option", strOption);
	if ("Echo" == strOption)
	{
		int32_t size = 0;
		obj.Get("size", size);
		if (size > 0 && size <= 1048576)
		{
			util::CJsonObject oRsp;
			oRsp.Add("code", 0);
			oRsp.Add("msg", "ok");
			oRsp.Add("size", size);
			std::string data(static_cast<size_t>(size), 'X');
			oRsp.Add("data", data);
			GetLabor()->SendToClient(stMsgShell, oInHttpMsg, oRsp.ToString());
		}
		else
		{
			Response(stMsgShell, oInHttpMsg, 0);
		}
	}
	else if ("TestHttpRequestCo" == strOption)
	{
		return TestHttpRequestCo(stMsgShell, oInHttpMsg);
	}
	else if ("TestHelloPoolCpu" == strOption)
	{
		return TestHelloPoolCpu(stMsgShell, oInHttpMsg);
	}
	else if ("TestHelloPoolBlock" == strOption)
	{
		return TestHelloPoolBlock(stMsgShell, oInHttpMsg);
	}
	else if ("TestHttps" == strOption)
	{
		// #130: HTTPS 出站测试 — 可选 url 参数，默认连本地测试服务器
		std::string url;
		obj.Get("url", url);
		if (url.empty()) url = "https://127.0.0.1:19995/";
		std::string host = url;
		auto pos = host.find("://"); if (pos != std::string::npos) host = host.substr(pos + 3);
		pos = host.find('/'); if (pos != std::string::npos) host = host.substr(0, pos);
		pos = host.find(':'); int port = (pos != std::string::npos) ? std::stoi(host.substr(pos + 1)) : 443;
		if (pos != std::string::npos) host = host.substr(0, pos);
		HttpMsg oReq;
		oReq.set_url(url);
		oReq.set_type(HTTP_REQUEST);
		oReq.set_method(HTTP_GET);
		oReq.set_http_major(1);
		oReq.set_http_minor(1);
		GetLabor()->SentTo(host, port, url, oReq, nullptr);
		Response(stMsgShell, oInHttpMsg, 0);
		return true;
	}
	else if ("TestHelloCoRedis" == strOption)
	{
		return TestHelloCoRedis(stMsgShell, oInHttpMsg, obj);
	}
	else if ("TestHelloCoMysql" == strOption)
	{
		return TestHelloCoMysql(stMsgShell, oInHttpMsg, obj);
	}
	else
	{
		LOG4_TRACE("no things to do");
		Response(stMsgShell, oInHttpMsg, 0);
	}
	return true;
}

bool ModuleHello::AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("body %s oInHttpMsg %s url:%s", oInHttpMsg.body().c_str(), oInHttpMsg.DebugString().c_str(), oInHttpMsg.url().c_str());
	for (auto&& p : oInHttpMsg.params())
	{
		LOG4_TRACE("p(%s,%s)", p.first.c_str(), p.second.c_str());
	}
	TestMsg(stMsgShell, oInHttpMsg);
	return true;
}

void ModuleHello::Response(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg, int iCode)
{
	util::CJsonObject oJsonObj;
	oJsonObj.Add("code", iCode);
	oJsonObj.Add("msg", "ok");
	GetLabor()->SendToClient(stMsgShell, oInHttpMsg, oJsonObj.ToString());
}

bool ModuleHello::TestHttpRequestCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	return net::LaunchCo(std::make_unique<HttpRequestCo>(stMsgShell, oInHttpMsg));
}

bool ModuleHello::TestHelloPoolCpu(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	return net::LaunchCo(stMsgShell, oInHttpMsg,
		[](net::StepCo20& s) -> net::AsyncTask { return HelloPoolCpuCo(s); });
}

bool ModuleHello::TestHelloPoolBlock(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	return net::LaunchCo(stMsgShell, oInHttpMsg,
		[](net::StepCo20& s) -> net::AsyncTask { return HelloPoolBlockCo(s); });
}

bool ModuleHello::TestHelloCoRedis(const net::tagMsgShell& stMsgShell,
                                   const HttpMsg& oInHttpMsg,
                                   const util::CJsonObject& obj)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	const std::string host = JsonStrOrDefault(obj, "redis_host", "127.0.0.1");
	const int port = JsonIntOrDefault(obj, "redis_port", 6379);
	return net::LaunchCo(stMsgShell, oInHttpMsg,
		[host, port](net::StepCo20& step) -> net::AsyncTask {
			return HelloCoRedisCo(step, host, port);
		});
}

bool ModuleHello::TestHelloCoMysql(const net::tagMsgShell& stMsgShell,
                                   const HttpMsg& oInHttpMsg,
                                   const util::CJsonObject& obj)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	const std::string h = JsonStrOrDefault(obj, "mysql_host", "127.0.0.1");
	const int p = JsonIntOrDefault(obj, "mysql_port", 3306);
	const std::string user = JsonStrOrDefault(obj, "mysql_user", "root");
	const std::string pwd = JsonStrOrDefault(obj, "mysql_password", "thunder");
	const std::string db = JsonStrOrDefault(obj, "mysql_db", "thunder_test");
	const std::string charset = JsonStrOrDefault(obj, "mysql_charset", "utf8mb4");
	const util::tagDbConnInfo dbConn = MakeTagDbConn(h, static_cast<unsigned int>(p), user, pwd, db, charset);
	return net::LaunchCo(stMsgShell, oInHttpMsg,
		[dbConn](net::StepCo20& step) -> net::AsyncTask {
			return HelloCoMysqlCo(step, dbConn);
		});
}

} /* namespace core */
