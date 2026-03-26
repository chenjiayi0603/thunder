#ifndef __MYSQL_CONN__
#define __MYSQL_CONN__

#include <cstddef>
#include <libev/ev.h>
#include <list>
#include <memory>
#include <mysql.h>
#include <string>

#include "Dbi.hpp"

namespace util
{

class MysqlAsyncConn;
class MysqlResSet;
struct SqlTask;

enum eSqlTaskOper {
	eSqlTaskOper_select,
	eSqlTaskOper_exec,
};

class MysqlHandler {
public:
	MysqlHandler() = default;
	virtual ~MysqlHandler() = default;
	virtual int on_execsql(MysqlAsyncConn* c, SqlTask* task) = 0;
	/// pResultSet 由回调方管理生命周期
	virtual int on_query(MysqlAsyncConn* c, SqlTask* task, MYSQL_RES* pResultSet) = 0;
};

struct SqlTask {
	SqlTask(const std::string& s, eSqlTaskOper o, MysqlHandler* h);
	virtual ~SqlTask() = default;
	std::string sql;
	eSqlTaskOper oper;
	std::unique_ptr<MysqlHandler> handler;
	int iErrno{};
	std::string errmsg;
};

MysqlAsyncConn* mysqlAsyncConnect(const char* ip, int port, const char* user, const char* passwd,
	const char* dbname, const char* dbcharacterset, struct ev_loop* loop);

class MysqlResSet {
public:
	MysqlResSet();
	MysqlResSet(MYSQL_RES* pResultSet, MYSQL* mysql);
	~MysqlResSet();
	void Init(MYSQL_RES* pResultSet, MYSQL* mysql);
	// 关闭结果集
	void Clear();
	// 获取结果集（填充 vecResultSet）
	int GetResultSet(T_vecResultSet& vecResultSet);
	// 返回结果集当前行 / 遍历行
	MYSQL_ROW GetRow();
	const MYSQL_RES* UseResult();
	// 当前行各列长度
	unsigned long* FetchLengths();
	unsigned int FetchFieldNum();
	unsigned int GetRowsNum();
	// 列元数据
	MYSQL_FIELD* FetchFields();
	// 取上一次数据库操作错误码 / 错误信息
	int GetErrno() const { return m_iErrno; }
	const std::string& GetError() const { return m_strError; }

private:
	MYSQL_RES* m_pResultSet{nullptr};
	MYSQL_ROW m_stCurrRow{nullptr};
	int m_dwRowCount{0};
	int m_dwFieldCount{0};
	int m_iErrno{0};
	std::string m_strError;
	MYSQL* m_pMysql{nullptr};
};

enum mysql_conn_state {
	NO_CONNECTED,
	CONNECT_WAITING,
	WAIT_OPERATE,
	EXECSQL_WAITING,
	QUERY_WAITING,
	STORE_WAITING
};

class MysqlAsyncConn {
public:
	MysqlAsyncConn();
	~MysqlAsyncConn();

	/// libev ev_io 嵌入本对象：从 watcher 反查连接（类型含非标准布局成员，offsetof 为编译器扩展支持场景）
	static MysqlAsyncConn* get_self_by_watcher(ev_io* watcher) noexcept {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
		return reinterpret_cast<MysqlAsyncConn*>(
			reinterpret_cast<char*>(watcher) - offsetof(MysqlAsyncConn, m_watcher));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
	}

	static void libev_io_cb(struct ev_loop* loop, ev_io* watcher, int event);

	int init(const char* ip, int port, const char* user, const char* passwd, const char* dbname,
		const char* dbcharacterset, struct ev_loop* loop);
	int init(const tagDbConnInfo& stDbConnInfo, struct ev_loop* loop);
	int event_to_mysql_status(int event);
	int mysql_status_to_event(int status);
	int wait_next_task(int libev_event = EV_WRITE);//目前只有主动发起的写事件

	void stop_task();

	int state_handle(struct ev_loop* loop, ev_io* watcher, int event);

	int check_error_reconnect(SqlTask* task);

	int connect_start();
	int connect_wait(struct ev_loop* loop, ev_io* watcher, int event);

	int execsql_start();
	int execsql_wait(struct ev_loop* loop, ev_io* watcher, int event);

	int query_start();
	int query_wait(struct ev_loop* loop, ev_io* watcher, int event);

	int store_result_start();
	int store_result_wait(struct ev_loop* loop, ev_io* watcher, int event);

	int close();

	void add_task(SqlTask* task) {
		if (task != nullptr) {
			m_SqlTaskList.push_back(task);
			wait_next_task();
		}
	}

	int task_size() const { return static_cast<int>(m_SqlTaskList.size()); }

	int all_task_size() const {
		return static_cast<int>(m_SqlTaskList.size() + (m_curSqlTask != nullptr ? 1U : 0U));
	}

	SqlTask* fetch_next_task() {
		if (m_SqlTaskList.empty()) {
			return nullptr;
		}
		SqlTask* task = m_SqlTaskList.front();
		m_SqlTaskList.pop_front();
		return task;
	}

	SqlTask* fetch_top_task() {
		if (m_SqlTaskList.empty()) {
			return nullptr;
		}
		return m_SqlTaskList.front();
	}

	MYSQL* GetMysql() { return &m_mysql; }

private:
	bool m_boInit{false};

	char m_ip[32]{};
	int m_port{0};
	char m_user[256]{};
	char m_passwd[256]{};
	char m_dbname[256]{};
	char m_dbcharacterset[32]{};

	int m_connIndex{0};
	MYSQL m_mysql{};
	MYSQL_RES* m_queryres{nullptr};
	MYSQL_ROW row{nullptr};

	ev_io m_watcher{};
	struct ev_loop* m_loop{nullptr};
	std::list<SqlTask*> m_SqlTaskList;
	mysql_conn_state m_state{NO_CONNECTED};
	SqlTask* m_curSqlTask{nullptr};
	int m_taskCounter{0};
};

}
#endif