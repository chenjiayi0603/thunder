#include "HelloCoRedis.hpp"
#include "coro/RedisAwaitable.hpp"
#include "coro/StepCo20.hpp"
#include "util/CommonUtils.hpp"

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
