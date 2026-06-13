#include "HelloCoMysql.hpp"
#include "coro/MySqlAwaitable.hpp"
#include "coro/StepCo20.hpp"
#include "util/CommonUtils.hpp"

net::AsyncTask HelloCoMysqlCo(net::StepCo20& step, util::tagDbConnInfo dbConn)
{
	net::MySqlCoHelper db(&step, std::move(dbConn));

	const std::string createSql =
	    "CREATE TABLE IF NOT EXISTS hello_co20_demo (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(128))";
	const std::string insertSql =
	    "INSERT INTO hello_co20_demo (v) VALUES ('co20_smoke')";
	const std::string selectSql =
	    "SELECT v FROM hello_co20_demo ORDER BY id DESC LIMIT 1";

	const net::MySqlReply createRsp = co_await db.Exec(createSql);
	const net::MySqlReply insertRsp = co_await db.Exec(insertSql);
	const net::MySqlReply selectRsp = co_await db.Query(selectSql);

	util::CJsonObject j;
	j.Add("option", "TestHelloCoMysql");
	j.Add("create_ok", createRsp.IsOk() ? 1 : 0);
	j.Add("insert_ok", insertRsp.IsOk() ? 1 : 0);
	j.Add("select_ok", selectRsp.IsOk() && selectRsp.result && !selectRsp.result->empty() ? 1 : 0);

	if (!createRsp.IsOk())
	{
		j.Add("create_err", createRsp.errMsg);
		j.Add("create_errno", static_cast<util::int64>(createRsp.errNo));
	}
	if (!insertRsp.IsOk())
	{
		j.Add("insert_err", insertRsp.errMsg);
		j.Add("insert_errno", static_cast<util::int64>(insertRsp.errNo));
	}
	if (!selectRsp.IsOk())
	{
		j.Add("select_err", selectRsp.errMsg);
		j.Add("select_errno", static_cast<util::int64>(selectRsp.errNo));
	}

	if (selectRsp.result && !selectRsp.result->empty())
	{
		const util::T_mapRow& row = (*selectRsp.result)[0];
		const auto it = row.find("v");
		if (it != row.end())
		{
			j.Add("last_v", it->second);
		}
	}
	step.ResponseToClient(200, j.ToString());
	co_return;
}
