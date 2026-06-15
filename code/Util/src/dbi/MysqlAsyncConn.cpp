#include "MysqlAsyncConn.h"

namespace util
{

SqlTask::SqlTask(const std::string& s, eSqlTaskOper o, MysqlHandler* h)
	: sql(s), oper(o), handler(h), iErrno(0)
{
}

MysqlAsyncConn::MysqlAsyncConn()
	: m_boInit(false)
	, m_ip{}
	, m_port(0)
	, m_user{}
	, m_passwd{}
	, m_dbname{}
	, m_dbcharacterset{}
	, m_connIndex(0)
	, m_mysql{}
	, m_queryres(nullptr)
	, row(nullptr)
	, m_watcher{}
	, m_loop(nullptr)
	, m_state(NO_CONNECTED)
	, m_curSqlTask(nullptr)
	, m_taskCounter(0)
{
}

MysqlAsyncConn::~MysqlAsyncConn()
{
	for (SqlTask* task : m_SqlTaskList) {
		delete task;
	}
	m_SqlTaskList.clear();
	delete m_curSqlTask;
	m_curSqlTask = nullptr;
}

int MysqlAsyncConn::init(const char* ip, int port, const char* user, const char* passwd,
	const char* dbname, const char* dbcharacterset, struct ev_loop* loop)
{
	strncpy(m_ip, ip, sizeof(m_ip));
	m_ip[sizeof(m_ip) - 1] = '\0';
	strncpy(m_user, user, sizeof(m_user));
	m_user[sizeof(m_user) - 1] = '\0';
	strncpy(m_passwd, passwd, sizeof(m_passwd));
	m_passwd[sizeof(m_passwd) - 1] = '\0';
	strncpy(m_dbname, dbname, sizeof(m_dbname));
	m_dbname[sizeof(m_dbname) - 1] = '\0';
	strncpy(m_dbcharacterset, dbcharacterset, sizeof(m_dbcharacterset));
	m_dbcharacterset[sizeof(m_dbcharacterset) - 1] = '\0';
	m_port = port;
	mysql_init(&m_mysql);
	// 让 MySQL Client 进入真正的非阻塞模式，
	// 否则 mysql_real_connect_start/mysql_real_query_start 的状态机可能无法按预期触发 cont 回调。
	// MariaDB Connector/C 3.4.x 期望 size_t*（栈大小，0=默认），不是 my_bool*。
	// 传 my_bool* 导致栈上 7 字节垃圾被读作 size_t → malloc 失败 → segfault。
	my_bool allow_invalid = 0;  // 0 = tls_allow_invalid_server_cert=1, 防止 auth 插件强制 use_ssl=1
	mysql_options(&m_mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &allow_invalid);
	size_t stacksize = 0;
	mysql_options(&m_mysql, MYSQL_OPT_NONBLOCK, &stacksize);
	unsigned int uiTimeOut = 3;
	mysql_options(&m_mysql, MYSQL_OPT_CONNECT_TIMEOUT, reinterpret_cast<char*>(&uiTimeOut));
	mysql_options(&m_mysql, MYSQL_OPT_COMPRESS, nullptr);
	mysql_options(&m_mysql, MYSQL_OPT_LOCAL_INFILE, nullptr);
	bool bBool = true;
	mysql_options(&m_mysql, MYSQL_OPT_RECONNECT, reinterpret_cast<char*>(&bBool));
	m_loop = loop;
	connect_start();
	m_boInit = true;
	return 0;
}

int MysqlAsyncConn::init(const tagDbConnInfo& stDbConnInfo, struct ev_loop* loop)
{
	return init(stDbConnInfo.m_szDbHost, stDbConnInfo.m_uiDbPort, stDbConnInfo.m_szDbUser,
		stDbConnInfo.m_szDbPwd, stDbConnInfo.m_szDbName, stDbConnInfo.m_szDbCharSet, loop);
}

void MysqlAsyncConn::libev_io_cb(struct ev_loop* loop, ev_io* watcher, int event)
{
	MysqlAsyncConn* conn = MysqlAsyncConn::get_self_by_watcher(watcher);
	conn->state_handle(loop, watcher, event);
}

int MysqlAsyncConn::event_to_mysql_status(int event)
{
	int status = 0;
	if (event & EV_READ) {
		status |= MYSQL_WAIT_READ;
	}
	if (event & EV_WRITE) {
		status |= MYSQL_WAIT_WRITE;
	}
	return status;
}

int MysqlAsyncConn::mysql_status_to_event(int status)
{
	int waitevent = 0;
	if (status & MYSQL_WAIT_READ) {
		waitevent |= EV_READ;
	}
	if (status & MYSQL_WAIT_WRITE) {
		waitevent |= EV_WRITE;
	}
	return waitevent;
}

int MysqlAsyncConn::state_handle(struct ev_loop* loop, ev_io* watcher, int event)
{
	if (m_state == CONNECT_WAITING) {
		connect_wait(loop, watcher, event);
	} else if (m_state == EXECSQL_WAITING) {
		execsql_wait(loop, watcher, event);
	} else if (m_state == QUERY_WAITING) {
		query_wait(loop, watcher, event);
	} else if (m_state == STORE_WAITING) {
		store_result_wait(loop, watcher, event);
	} else if (m_state == WAIT_OPERATE) {
		if (m_curSqlTask) {
			delete m_curSqlTask;
			m_curSqlTask = nullptr;
		}
		if ((m_curSqlTask = fetch_next_task()) != nullptr) {
			if (eSqlTaskOper_select == m_curSqlTask->oper) {
				query_start();
			} else {
				execsql_start();
			}
		} else {
			stop_task();
		}
	}
	return 0;
}

int MysqlAsyncConn::wait_next_task(int libev_event)
{
	if (all_task_size() > 0) {
		ev_io_stop(m_loop, &m_watcher);
		ev_io_set(&m_watcher, m_watcher.fd, m_watcher.events | libev_event);
		ev_io_start(m_loop, &m_watcher);
	}
	return 0;
}

void MysqlAsyncConn::stop_task()
{
	ev_io_stop(m_loop, &m_watcher);
}

int MysqlAsyncConn::check_error_reconnect(SqlTask* task)
{
	/*
	 * 2006 (CR_SERVER_GONE_ERROR) : MySQL服务器不可用
	 * 2013 (CR_SERVER_LOST) : 查询过程中丢失了与MySQL服务器的连接
	 * mysql_ping 这个函数的返回值和文档说明有出入，返回0并不能保证连接是可用的
	 *
	 * Bug 3 修复：重连前把当前任务重新入队首，使重连成功后的 WAIT_OPERATE 状态
	 * 能正确 fetch_next_task() 并执行，而不是悄悄丢弃（原实现调用 connect_start()
	 * 后直接 return 0，WAIT_OPERATE 中执行 delete m_curSqlTask 导致任务丢失）。
	 */
	if (task->iErrno == 2006 || task->iErrno == 2013) {
		// 重置错误状态，放回队首，重连成功后重试
		task->iErrno = 0;
		task->errmsg.clear();
		m_SqlTaskList.push_front(task);
		m_curSqlTask = nullptr;
		connect_start();
		return 0;
	}
	return 1;
}

int MysqlAsyncConn::connect_start()
{
	MYSQL* ret = nullptr;
	int status = mysql_real_connect_start(&ret, &m_mysql, m_ip, m_user, m_passwd, m_dbname, m_port, nullptr, 0);
	if (0 != status) {
		m_state = CONNECT_WAITING;
		int waitevent = mysql_status_to_event(status);
		if (!m_boInit) {
			int socket = mysql_get_socket(&m_mysql);
			ev_io_init(&m_watcher, libev_io_cb, socket, waitevent);
			ev_io_start(m_loop, &m_watcher);
		} else {
			ev_io_stop(m_loop, &m_watcher);
			ev_io_set(&m_watcher, m_watcher.fd, m_watcher.events | waitevent);
			ev_io_start(m_loop, &m_watcher);
		}
	} else {
		mysql_set_character_set(&m_mysql, m_dbcharacterset);
		m_state = WAIT_OPERATE;
		// 同步连接：仍需初始化 watcher，否则后续 wait_next_task()
		// 因 m_watcher.fd==0 把 MySQL 事件绑定到 fd 0（stdin）→ 永不触发。
		if (!m_boInit) {
			int socket = mysql_get_socket(&m_mysql);
			if (socket > 0) {
				ev_io_init(&m_watcher, libev_io_cb, socket, EV_WRITE);
			}
		}
		wait_next_task();
	}
	return 0;
}

int MysqlAsyncConn::connect_wait(struct ev_loop* loop, ev_io* watcher, int event)
{
	MYSQL* ret = nullptr;
	int status = event_to_mysql_status(event);
	status = mysql_real_connect_cont(&ret, &m_mysql, status);
	if (0 == status) {
		mysql_set_character_set(&m_mysql, m_dbcharacterset);
		m_state = WAIT_OPERATE;
		wait_next_task();
	} else {
		if (mysql_errno(&m_mysql) > 0) {
			if (m_curSqlTask == nullptr) {
				m_curSqlTask = fetch_next_task();
			}
			if (m_curSqlTask) {
				m_curSqlTask->iErrno = mysql_errno(&m_mysql);
				m_curSqlTask->errmsg = mysql_error(&m_mysql);
				if (0 == check_error_reconnect(m_curSqlTask)) {
					return 0;
				}
				m_curSqlTask->handler->on_execsql(this, m_curSqlTask);
				delete m_curSqlTask;
				m_curSqlTask = nullptr;
				wait_next_task();
			}
			return 1;
		}
		// Bug 5 修复：connect_wait 返回非零 status 表示需要继续等待 I/O，
		// 必须重新设置 watcher 事件，否则后续 I/O 事件永不触发 → 连接挂起。
		int waitevent = mysql_status_to_event(status);
		m_state = CONNECT_WAITING;
		ev_io_stop(m_loop, &m_watcher);
		ev_io_set(&m_watcher, m_watcher.fd, m_watcher.events | waitevent);
		ev_io_start(m_loop, &m_watcher);
	}
	return 0;
}

int MysqlAsyncConn::execsql_start()
{
	if (m_curSqlTask) {
		int iRet = 0;
		int status = mysql_real_query_start(&iRet, &m_mysql, m_curSqlTask->sql.c_str(), m_curSqlTask->sql.size());
		if (0 != iRet) {
			m_curSqlTask->iErrno = mysql_errno(&m_mysql);
			m_curSqlTask->errmsg = mysql_error(&m_mysql);
			if (0 == check_error_reconnect(m_curSqlTask)) {
				return 0;
			}
			m_curSqlTask->handler->on_execsql(this, m_curSqlTask);
			m_state = WAIT_OPERATE;
			wait_next_task();
			return 1;
		}
		if (status) {
			m_state = EXECSQL_WAITING;
			int waitevent = mysql_status_to_event(status);
			ev_io_stop(m_loop, &m_watcher);
			ev_io_set(&m_watcher, m_watcher.fd, m_watcher.events | waitevent);
			ev_io_start(m_loop, &m_watcher);
		} else {
			m_curSqlTask->handler->on_execsql(this, m_curSqlTask);
			m_state = WAIT_OPERATE;
			wait_next_task();
		}
	}
	return 0;
}

int MysqlAsyncConn::execsql_wait(struct ev_loop* loop, ev_io* watcher, int event)
{
	(void)loop;
	(void)watcher;
	if (m_curSqlTask) {
		int iRet = 0;
		int status = event_to_mysql_status(event);
		status = mysql_real_query_cont(&iRet, &m_mysql, status);
		if (0 != iRet) {
			m_curSqlTask->iErrno = mysql_errno(&m_mysql);
			m_curSqlTask->errmsg = mysql_error(&m_mysql);
			if (0 == check_error_reconnect(m_curSqlTask)) {
				return 0;
			}
			m_curSqlTask->handler->on_execsql(this, m_curSqlTask);
			m_state = WAIT_OPERATE;
			wait_next_task();
			return 1;
		}
		if (status != 0) {
			m_state = EXECSQL_WAITING;
		} else {
			m_curSqlTask->handler->on_execsql(this, m_curSqlTask);
			m_state = WAIT_OPERATE;
			wait_next_task();
		}
	}
	return 0;
}

int MysqlAsyncConn::query_start()
{
	if (m_curSqlTask) {
		int iRet = 0;
		int status = mysql_real_query_start(&iRet, &m_mysql, m_curSqlTask->sql.c_str(), m_curSqlTask->sql.size());
		if (0 != iRet) {
			m_curSqlTask->iErrno = mysql_errno(&m_mysql);
			m_curSqlTask->errmsg = mysql_error(&m_mysql);
			if (0 == check_error_reconnect(m_curSqlTask)) {
				return 0;
			}
			m_curSqlTask->handler->on_query(this, m_curSqlTask, m_queryres);
			m_state = WAIT_OPERATE;
			wait_next_task();
			return 1;
		}
		if (status) {
			m_state = QUERY_WAITING;
			int waitevent = mysql_status_to_event(status);
			ev_io_stop(m_loop, &m_watcher);
			ev_io_set(&m_watcher, m_watcher.fd, m_watcher.events | waitevent);
			ev_io_start(m_loop, &m_watcher);
		} else {
			store_result_start();
		}
	}
	return 0;
}

int MysqlAsyncConn::query_wait(struct ev_loop* loop, ev_io* watcher, int event)
{
	(void)loop;
	(void)watcher;
	if (m_curSqlTask) {
		int iRet = 0;
		int status = event_to_mysql_status(event);
		status = mysql_real_query_cont(&iRet, &m_mysql, status);
		if (0 != iRet) {
			m_curSqlTask->iErrno = mysql_errno(&m_mysql);
			m_curSqlTask->errmsg = mysql_error(&m_mysql);
			if (0 == check_error_reconnect(m_curSqlTask)) {
				return 0;
			}
			m_curSqlTask->handler->on_query(this, m_curSqlTask, m_queryres);
			m_state = WAIT_OPERATE;
			wait_next_task();
			return 1;
		}
		if (status != 0) {
			m_state = QUERY_WAITING;
		} else {
			store_result_start();
		}
	}
	return 0;
}

int MysqlAsyncConn::store_result_start()
{
	if (m_curSqlTask) {
		int status = mysql_store_result_start(&m_queryres, &m_mysql);
		if (status) {
			m_state = STORE_WAITING;
			int waitevent = mysql_status_to_event(status);
			ev_io_stop(m_loop, &m_watcher);
			ev_io_set(&m_watcher, m_watcher.fd, m_watcher.events | waitevent);
			ev_io_start(m_loop, &m_watcher);
		} else {
			m_curSqlTask->handler->on_query(this, m_curSqlTask, m_queryres);
			m_state = WAIT_OPERATE;
			wait_next_task();
		}
	}
	return 0;
}

int MysqlAsyncConn::store_result_wait(struct ev_loop* loop, ev_io* watcher, int event)
{
	(void)loop;
	(void)watcher;
	if (m_curSqlTask) {
		int status = event_to_mysql_status(event);
		status = mysql_store_result_cont(&m_queryres, &m_mysql, status);
		if (status != 0) {
			m_state = STORE_WAITING;
		} else {
			if (m_queryres && m_curSqlTask) {
				m_curSqlTask->handler->on_query(this, m_curSqlTask, m_queryres);
			}
			m_state = WAIT_OPERATE;
			wait_next_task();
		}
	}
	return 0;
}

int MysqlAsyncConn::close()
{
	mysql_close(&m_mysql);
	return 0;
}

MysqlResSet::MysqlResSet()
	: m_pResultSet(nullptr)
	, m_stCurrRow(nullptr)
	, m_dwRowCount(0)
	, m_dwFieldCount(0)
	, m_iErrno(0)
	, m_pMysql(nullptr)
{
}

MysqlResSet::MysqlResSet(MYSQL_RES* pResultSet, MYSQL* mysql)
{
	Init(pResultSet, mysql);
}

MysqlResSet::~MysqlResSet()
{
	Clear();
}

void MysqlResSet::Init(MYSQL_RES* pResultSet, MYSQL* mysql)
{
	m_pMysql = mysql;
	m_stCurrRow = nullptr;
	if (pResultSet != m_pResultSet) {
		if (m_pResultSet) {
			delete m_pResultSet;
		}
		m_pResultSet = pResultSet;
	}
	if (m_pResultSet) {
		m_dwFieldCount = static_cast<int>(mysql_num_fields(m_pResultSet));
		m_dwRowCount = static_cast<int>(mysql_num_rows(m_pResultSet));
	} else {
		m_dwRowCount = 0;
		m_dwFieldCount = 0;
	}
	if (m_pMysql) {
		m_iErrno = mysql_errno(m_pMysql);
		m_strError = mysql_error(m_pMysql);
	} else {
		m_iErrno = 0;
		m_strError.clear();
	}
}

int MysqlResSet::GetResultSet(T_vecResultSet& vecResultSet)
{
	if (!m_pResultSet) {
		return 0;
	}
	MYSQL_FIELD* fields = mysql_fetch_fields(m_pResultSet);
	while ((m_stCurrRow = mysql_fetch_row(m_pResultSet)) != nullptr) {
		T_mapRow mapRow;
		for (int i = 0; i < m_dwFieldCount; ++i) {
			if (m_stCurrRow[i] != nullptr) {
				mapRow[fields[i].name] = m_stCurrRow[i];
			} else {
				mapRow[fields[i].name] = "";
			}
		}
		vecResultSet.push_back(mapRow);
	}
	return static_cast<int>(vecResultSet.size());
}

void MysqlResSet::Clear()
{
	if (m_pResultSet) {
		mysql_free_result(m_pResultSet);
		m_pResultSet = nullptr;
	}
}

unsigned long* MysqlResSet::FetchLengths()
{
	if (!m_pResultSet) {
		return nullptr;
	}
	return mysql_fetch_lengths(m_pResultSet);
}

unsigned int MysqlResSet::FetchFieldNum()
{
	if (!m_pResultSet) {
		return 0;
	}
	return mysql_num_fields(m_pResultSet);
}

unsigned int MysqlResSet::GetRowsNum()
{
	if (!m_pResultSet) {
		return 0;
	}
	return static_cast<unsigned int>(mysql_num_rows(m_pResultSet));
}

MYSQL_ROW MysqlResSet::GetRow()
{
	if (!m_pResultSet) {
		return nullptr;
	}
	m_stCurrRow = mysql_fetch_row(m_pResultSet);
	return m_stCurrRow;
}

const MYSQL_RES* MysqlResSet::UseResult()
{
	return m_pResultSet;
}

MysqlAsyncConn* mysqlAsyncConnect(const char* ip, int port, const char* user, const char* passwd, const char* dbname,
	const char* dbcharacterset, struct ev_loop* loop)
{
	MysqlAsyncConn* pMysqlConn = new MysqlAsyncConn();
	int nRet = pMysqlConn->init(ip, port, user, passwd, dbname, dbcharacterset, loop);
	if (0 != nRet) {
		delete pMysqlConn;
		return nullptr;
	}
	return pMysqlConn;
}

}
