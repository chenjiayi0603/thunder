/*******************************************************************************
 * Project:  Hello
 * @file     ModuleHello.cpp
 * @brief    精简：Echo、C++20 协程 HTTP 演示
 *           协程 + 线程池（设计向伪代码，例 A/例 B）见同目录 StepCo20-threadpool-examples.md
 ******************************************************************************/
#include <map>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>
#include <memory>

#include "util/CommonUtils.hpp"
#include "util/StringCoder.hpp"
#include "ModuleHello.hpp"
#include "HttpRequestCo.hpp"
#include "Interface.hpp"
#include "coro/StepCo20Func.hpp"
#include "coro/RedisAwaitable.hpp"
#include "dbi/MysqlDbi.hpp"
#include "coro/ThreadPoolAwaitable.hpp"
#include "labor/WorkerThreadPool.hpp"
#include "dbi/Dbi.hpp"

MUDULE_CREATE(core::ModuleHello);

namespace core
{

namespace hello_co_demo
{

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

net::AsyncTask HelloCoRedisCo(net::StepCo20& step, std::string redisHost, int redisPort)
{
	net::RedisCoHelper r(&step, redisHost, redisPort);
	const std::string key = "hello_co20_demo";
	const std::string val = "ok_from_co20";
	const net::RedisReply setRsp = co_await r.Set(key, val);
	const net::RedisReply getRsp = co_await r.Get(key);

	util::CJsonObject j;
	j.Add("option", "TestHelloCoRedis");
	j.Add("redis_host", redisHost);
	j.Add("redis_port", static_cast<util::int64>(redisPort));
	j.Add("set_ok", setRsp.IsOk() ? 1 : 0);
	j.Add("get_ok", getRsp.IsOk() ? 1 : 0);
	if (getRsp.IsOk())
	{
		j.Add("get_value", getRsp.AsString());
	}
	if (!setRsp.IsOk())
	{
		j.Add("set_err", setRsp.errMsg);
		j.Add("set_errno", static_cast<util::int64>(setRsp.errNo));
	}
	if (!getRsp.IsOk())
	{
		j.Add("get_err", getRsp.errMsg);
		j.Add("get_errno", static_cast<util::int64>(getRsp.errNo));
	}
	step.ResponseToClient(200, j.ToString());
	co_return;
}

net::AsyncTask HelloCoMysqlCo(net::StepCo20& step, util::tagDbConnInfo dbConn)
{
	// 为了让冒烟测试稳定，改为同步 DBI（避免 MysqlAsyncConn 事件回调时序问题）。
	util::CMysqlDbi db(dbConn.m_szDbHost,
	                    dbConn.m_szDbUser,
	                    dbConn.m_szDbPwd,
	                    dbConn.m_szDbName,
	                    dbConn.m_szDbCharSet,
	                    dbConn.m_uiDbPort);

	const std::string createSql =
	    "CREATE TABLE IF NOT EXISTS hello_co20_demo (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(128))";
	const std::string insertSql =
	    "INSERT INTO hello_co20_demo (v) VALUES ('co20_smoke')";
	const std::string selectSql =
	    "SELECT v FROM hello_co20_demo ORDER BY id DESC LIMIT 1";

	const int createRet = db.ExecSql(createSql);
	const int createErrno = db.GetErrno();
	const std::string createErr = db.GetError();

	const int insertRet = db.ExecSql(insertSql);
	const int insertErrno = db.GetErrno();
	const std::string insertErr = db.GetError();

	util::T_vecResultSet vec;
	const int selectRet = db.ExecSql(selectSql, vec);
	const int selectErrno = db.GetErrno();
	const std::string selectErr = db.GetError();

	util::CJsonObject j;
	j.Add("option", "TestHelloCoMysql");
	j.Add("create_ok", createRet == 0 ? 1 : 0);
	j.Add("insert_ok", insertRet == 0 ? 1 : 0);
	j.Add("select_ok", selectRet == 0 && !vec.empty() ? 1 : 0);

	if (createRet != 0)
	{
		j.Add("create_err", createErr);
		j.Add("create_errno", static_cast<util::int64>(createErrno));
	}
	if (insertRet != 0)
	{
		j.Add("insert_err", insertErr);
		j.Add("insert_errno", static_cast<util::int64>(insertErrno));
	}
	if (selectRet != 0)
	{
		j.Add("select_err", selectErr);
		j.Add("select_errno", static_cast<util::int64>(selectErrno));
	}

	if (!vec.empty())
	{
		const util::T_mapRow& row = vec[0];
		const auto it = row.find("v");
		if (it != row.end())
		{
			j.Add("last_v", it->second);
		}
	}
	step.ResponseToClient(200, j.ToString());
	co_return;
}

} // namespace hello_co_demo

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
		return TestHttpRequestCo(stMsgShell,oInHttpMsg);
	}
	else if ("TestHelloPoolCpu" == strOption)
	{
		return TestHelloPoolCpu(stMsgShell, oInHttpMsg);
	}
	else if ("TestHelloPoolBlock" == strOption)
	{
		return TestHelloPoolBlock(stMsgShell, oInHttpMsg);
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
		Response(stMsgShell,oInHttpMsg,0);
	}
	return true;
}

bool ModuleHello::AnyMessage(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	// Fast path: /hello/raw — zero overhead: skip JSON, skip CJsonObject, fixed string
	// Used for pure I/O benchmark comparison with Nginx
	// 进一步优化: 使用 SendToClientFast 绕过 protobuf HttpMsg 构建 + vsnprintf 编码
	if (oInHttpMsg.path() == "/hello/raw")
	{
		static const char kRawResponse[] = "{\"code\":0,\"msg\":\"ok\"}";
		GetLabor()->SendToClientFast(stMsgShell, kRawResponse, sizeof(kRawResponse) - 1);
		return true;
	}

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

namespace
{

net::AsyncTask HelloPoolCpuCo(net::StepCo20& step)
{
	std::vector<uint8_t> buf(256 * 1024, static_cast<uint8_t>(3));
	const uint64_t checksum = co_await net::MakePoolOffloadAwaiter(
		&step,
		[](std::vector<uint8_t> b) -> uint64_t {
			// 运行于线程池子线程（非 Worker/libev 线程）：勿用 GetLabor、ResponseToClient、
			// 未同步的共享可变状态及非线程安全接口；仅处理入参副本，结果通过返回值传出。
			uint64_t s = 0;
			for (uint8_t x : b)
			{
				s += x;
			}
			return s;
		},
		std::move(buf));
	util::CJsonObject j;
	j.Add("option", "TestHelloPoolCpu");
	j.Add("checksum", static_cast<util::int64>(checksum));
	step.ResponseToClient(200, j.ToString());
	co_return;
}

net::AsyncTask HelloPoolBlockCo(net::StepCo20& step)
{
	const int delay_ms = 80;
	const int delay_ms2 = delay_ms + 1;
	const int result = co_await net::MakePoolOffloadAwaiter(
		&step,
		[](int d1, int d2) -> int {
			// 典型场景：在线程池子线程中调用无状态、会阻塞的外部 IO 同步 SDK 或函数（此处 sleep 仅作演示）。
			// 同上约束：不得访问 Step/事件线程资源或未经同步的共享数据。
			(void)d2; // 仅为参与计算展示：实际阻塞用 d1
			std::this_thread::sleep_for(std::chrono::milliseconds(d1));
			return d1 + d2;
		},
		delay_ms, delay_ms2);
	util::CJsonObject j;
	j.Add("option", "TestHelloPoolBlock");
	j.Add("slept_ms", delay_ms);
	j.Add("result", result);
	step.ResponseToClient(200, j.ToString());
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

bool ModuleHello::TestHttpRequestCo(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
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
	const std::string host = hello_co_demo::JsonStrOrDefault(obj, "redis_host", "127.0.0.1");
	const int port = hello_co_demo::JsonIntOrDefault(obj, "redis_port", 6379);
	return net::LaunchCo(stMsgShell, oInHttpMsg,
		[host, port](net::StepCo20& step) -> net::AsyncTask {
			return hello_co_demo::HelloCoRedisCo(step, host, port);
		});
}

bool ModuleHello::TestHelloCoMysql(const net::tagMsgShell& stMsgShell,
                                   const HttpMsg& oInHttpMsg,
                                   const util::CJsonObject& obj)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	const std::string h = hello_co_demo::JsonStrOrDefault(obj, "mysql_host", "127.0.0.1");
	const int p = hello_co_demo::JsonIntOrDefault(obj, "mysql_port", 3306);
	const std::string user = hello_co_demo::JsonStrOrDefault(obj, "mysql_user", "root");
	const std::string pwd = hello_co_demo::JsonStrOrDefault(obj, "mysql_password", "thunder");
	const std::string db = hello_co_demo::JsonStrOrDefault(obj, "mysql_db", "thunder_test");
	const std::string charset = hello_co_demo::JsonStrOrDefault(obj, "mysql_charset", "utf8mb4");
	const util::tagDbConnInfo dbConn = hello_co_demo::MakeTagDbConn(h, static_cast<unsigned int>(p), user,
	                                                                pwd, db, charset);
	return net::LaunchCo(stMsgShell, oInHttpMsg,
		[dbConn](net::StepCo20& step) -> net::AsyncTask {
			return hello_co_demo::HelloCoMysqlCo(step, dbConn);
		});
}

} /* namespace core */
