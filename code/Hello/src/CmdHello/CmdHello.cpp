/*******************************************************************************
 * Cmd 20001：与 ModuleHello::TestMsg 同构（JSON option），供 CodecWebSocket 等接入
 ******************************************************************************/
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>

#include "CmdHello.hpp"

#include "util/CommonUtils.hpp"
#include "coro/StepCo20Func.hpp"
#include "coro/RedisAwaitable.hpp"
#include "coro/ThreadPoolAwaitable.hpp"
#include "dbi/MysqlDbi.hpp"
#include "dbi/Dbi.hpp"
#include "labor/WorkerThreadPool.hpp"

MUDULE_CREATE(core::CmdHello);

namespace core
{

namespace
{

void ReplyJson(net::Labor* labor, const net::tagMsgShell& sh, const MsgHead& req, const std::string& json)
{
	MsgHead oOut;
	MsgBody oBody;
	// 与 SendToClient(str) 一致走 body；CodecWebSocketJson::Encode 将 body 按 UTF-8 JSON 原样入帧
	oBody.set_body(json);
	oOut.set_seq(req.seq());
	oOut.set_cmd(req.cmd() + 1);
	oOut.set_msgbody_len(oBody.ByteSize());
	labor->SendTo(sh, oOut, oBody);
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

net::AsyncTask HelloPoolCpuCo(net::StepCo20& step)
{
	std::vector<uint8_t> buf(256 * 1024, static_cast<uint8_t>(3));
	const uint64_t checksum = co_await net::MakePoolOffloadAwaiter(
	    &step, net::ThunderWorkerThreadPool(),
	    [](std::vector<uint8_t> b) -> uint64_t {
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
	    &step, net::ThunderWorkerThreadPool(),
	    [](int d1, int d2) -> int {
		    (void)d2;
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

/// 与 ModuleHello/HttpRequestCo::AsyncBody 一致，通过 StepCo20::ResponseToClient 回包（WS 走 sbody）
net::AsyncTask HttpRequestCoWsAsync(net::StepCo20& step)
{
	uint32_t testVal = 0;
	try
	{
		LOG4_TRACE("HttpRequestCoWsAsync state 0: baidu, testVal:%u", ++testVal);
		bool bSuccess = co_await step.HttpGetAsync("http://www.baidu.com/");
		if (!bSuccess)
		{
			LOG4_ERROR("HttpGet baidu error");
			step.ResponseToClient(500, R"({"code":1,"msg":"http baidu failed"})");
			co_return;
		}
		LOG4_TRACE("HttpRequestCoWsAsync state 1: sogou, testVal:%u", ++testVal);
		bSuccess = co_await step.HttpGetAsync("http://www.sogou.com/");
		if (!bSuccess)
		{
			LOG4_ERROR("HttpGet sogou error");
			step.ResponseToClient(500, R"({"code":1,"msg":"http sogou failed"})");
			co_return;
		}
		LOG4_TRACE("HttpRequestCoWsAsync state 2: alipay, testVal:%u", ++testVal);
		bSuccess = co_await step.HttpGetAsync("http://www.alipay.com/");
		if (!bSuccess)
		{
			LOG4_ERROR("HttpGet alipay error");
			step.ResponseToClient(500, R"({"code":1,"msg":"http alipay failed"})");
			co_return;
		}
		LOG4_TRACE("HttpRequestCoWsAsync state 3: qq, testVal:%u", ++testVal);
		bSuccess = co_await step.HttpGetAsync("http://www.qq.com/");
		if (!bSuccess)
		{
			LOG4_ERROR("HttpGet qq error");
			step.ResponseToClient(500, R"({"code":1,"msg":"http qq failed"})");
			co_return;
		}
		util::CJsonObject oJsonObj;
		oJsonObj.Add("code", 0);
		oJsonObj.Add("msg", "ok");
		oJsonObj.Add("testVal", static_cast<util::int64>(testVal));
		oJsonObj.Add("stepType", "HttpRequestCo_StepCo20_ws");
		step.ResponseToClient(200, oJsonObj.ToString());
	}
	catch (const std::exception& e)
	{
		LOG4_ERROR("HttpRequestCoWsAsync exception: %s", e.what());
		step.ResponseToClient(500, R"({"code":1,"msg":"exception"})");
	}
	catch (...)
	{
		LOG4_ERROR("HttpRequestCoWsAsync unknown exception");
		step.ResponseToClient(500, R"({"code":1,"msg":"unknown"})");
	}
	co_return;
}

} // namespace

bool CmdHello::Init()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	return true;
}

bool CmdHello::AnyMessage(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                          const MsgBody& oInMsgBody)
{
	util::CJsonObject obj;
	std::string raw;
	if (!oInMsgBody.body().empty())
	{
		raw.assign(oInMsgBody.body().data(), oInMsgBody.body().size());
	}
	if (!obj.Parse(raw))
	{
		LOG4_WARN("CmdHello parse failed, raw_len=%zu", raw.size());
		ReplyJson(GetLabor(), stMsgShell, oInMsgHead, R"({"code":1,"msg":"invalid json"})");
		return false;
	}

	std::string strOption;
	obj.Get("option", strOption);
	net::Labor* labor = GetLabor();

	if (strOption == "Echo")
	{
		util::CJsonObject oJsonObj;
		oJsonObj.Add("code", 0);
		oJsonObj.Add("msg", "ok");
		ReplyJson(labor, stMsgShell, oInMsgHead, oJsonObj.ToString());
		return true;
	}
	if (strOption == "TestHttpRequestCo")
	{
		return net::LaunchCo(stMsgShell, oInMsgHead,
		                     [](net::StepCo20& s) -> net::AsyncTask { return HttpRequestCoWsAsync(s); });
	}
	if (strOption == "TestHelloPoolCpu")
	{
		return net::LaunchCo(stMsgShell, oInMsgHead,
		                     [](net::StepCo20& s) -> net::AsyncTask { return HelloPoolCpuCo(s); });
	}
	if (strOption == "TestHelloPoolBlock")
	{
		return net::LaunchCo(stMsgShell, oInMsgHead,
		                     [](net::StepCo20& s) -> net::AsyncTask { return HelloPoolBlockCo(s); });
	}
	if (strOption == "TestHelloCoRedis")
	{
		const std::string host = JsonStrOrDefault(obj, "redis_host", "127.0.0.1");
		const int port = JsonIntOrDefault(obj, "redis_port", 6379);
		return net::LaunchCo(stMsgShell, oInMsgHead,
		                     [host, port](net::StepCo20& step) -> net::AsyncTask {
			                     return HelloCoRedisCo(step, host, port);
		                     });
	}
	if (strOption == "TestHelloCoMysql")
	{
		const std::string h = JsonStrOrDefault(obj, "mysql_host", "127.0.0.1");
		const int p = JsonIntOrDefault(obj, "mysql_port", 3306);
		const std::string user = JsonStrOrDefault(obj, "mysql_user", "root");
		const std::string pwd = JsonStrOrDefault(obj, "mysql_password", "thunder");
		const std::string db = JsonStrOrDefault(obj, "mysql_db", "thunder_test");
		const std::string charset = JsonStrOrDefault(obj, "mysql_charset", "utf8mb4");
		const util::tagDbConnInfo dbConn = MakeTagDbConn(h, static_cast<unsigned int>(p), user, pwd, db, charset);
		return net::LaunchCo(stMsgShell, oInMsgHead,
		                     [dbConn](net::StepCo20& step) -> net::AsyncTask {
			                     return HelloCoMysqlCo(step, dbConn);
		                     });
	}

	util::CJsonObject oJsonObj;
	oJsonObj.Add("code", 0);
	oJsonObj.Add("msg", "ok");
	ReplyJson(labor, stMsgShell, oInMsgHead, oJsonObj.ToString());
	return true;
}

} // namespace core
