/*******************************************************************************
 * Project:  Net
 * @file     MysqlStep.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年8月15日
 * @note
 * Modify history:
 ******************************************************************************/
#include "MysqlStep.hpp"
#include "labor/Worker.hpp"

namespace net
{
static uint64 uiMysqlStepRegisterCounter = 0;
static uint64 uiMysqlStepDectructCounter = 0;

MysqlStep::MysqlStep(const std::string& strHost, int iPort,const std::string& dbname,
		const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut)
{
	Init(strHost,iPort,dbname,user,passwd,dbcharacterset,uiTimeOut);
}

MysqlStep::MysqlStep(const util::tagDbConnInfo &dbConnInfo)
{
	Init(dbConnInfo.m_szDbHost,dbConnInfo.m_uiDbPort,dbConnInfo.m_szDbName,
				dbConnInfo.m_szDbUser,dbConnInfo.m_szDbPwd,dbConnInfo.m_szDbCharSet,dbConnInfo.uiTimeOut);
}

MysqlStep::MysqlStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead,const util::tagDbConnInfo &dbConnInfo)
:Step(stReqMsgShell,oReqMsgHead)
{
	Init(dbConnInfo.m_szDbHost,dbConnInfo.m_uiDbPort,dbConnInfo.m_szDbName,
			dbConnInfo.m_szDbUser,dbConnInfo.m_szDbPwd,dbConnInfo.m_szDbCharSet,dbConnInfo.uiTimeOut);
}
MysqlStep::MysqlStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead,const std::string& strHost, int iPort,const std::string& dbname,
		const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut)
:Step(stReqMsgShell,oReqMsgHead)
{
	Init(strHost,iPort,dbname,user,passwd,dbcharacterset,uiTimeOut);
}
MysqlStep::MysqlStep(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg,const std::string& strHost, int iPort,const std::string& dbname,
		const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut)
:Step(stReqMsgShell)
{
	(void)oInHttpMsg;
	Init(strHost,iPort,dbname,user,passwd,dbcharacterset,uiTimeOut);
}
MysqlStep::MysqlStep(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg,const util::tagDbConnInfo &dbConnInfo)
:Step(stReqMsgShell)
{
	(void)oInHttpMsg;
	Init(dbConnInfo.m_szDbHost,dbConnInfo.m_uiDbPort,dbConnInfo.m_szDbName,
				dbConnInfo.m_szDbUser,dbConnInfo.m_szDbPwd,dbConnInfo.m_szDbCharSet,dbConnInfo.uiTimeOut);
}

void MysqlStep::Init(const std::string& strHost, int iPort,const std::string& dbname,
		const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut)
{
	m_strHost = strHost;
	m_iPort = iPort;
	m_strDbName = dbname;
	m_strUser = user;
	m_strPasswd = passwd;
	m_dbcharacterset = dbcharacterset;
	m_uiTimeOut = uiTimeOut;

	m_pMysqlResSet = std::make_unique<util::MysqlResSet>();
}

void MysqlStep::SetConf(const util::tagDbConnInfo &dbConnInfo)
{
	m_strHost = dbConnInfo.m_szDbHost;
	m_iPort = dbConnInfo.m_uiDbPort;
	m_strDbName = dbConnInfo.m_szDbName;
	m_strUser = dbConnInfo.m_szDbUser;
	m_strPasswd = dbConnInfo.m_szDbPwd;
	m_dbcharacterset = dbConnInfo.m_szDbCharSet;
	m_uiTimeOut = dbConnInfo.uiTimeOut;
}

void MysqlStep::SetConf(const std::string& strHost, int iPort,const std::string& dbname,
		const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut)
{
	m_strHost = strHost;
	m_iPort = iPort;
	m_strDbName = dbname;
	m_strUser = user;
	m_strPasswd = passwd;
	m_dbcharacterset = dbcharacterset;
	m_uiTimeOut = uiTimeOut;
}

MysqlStep::~MysqlStep()
{
	LOG4_TRACE("%s() uiMysqlStepDectructCounter:%llu",__FUNCTION__,++uiMysqlStepDectructCounter);
	m_pMysqlResSet.reset();
}

E_CMD_STATUS MysqlStep::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
	(void)strErrShow;
	if (0 != iErrno)
	{
		m_iErrno = iErrno;
		m_strErrMsg = strErrMsg;
		LOG4_WARN("%s() Fail strStepDesc(%s)", __FUNCTION__, m_strStepDesc.c_str());
		return STATUS_CMD_FAULT;
	}
	return STATUS_CMD_COMPLETED;
}

E_CMD_STATUS MysqlStep::Callback(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, void* data)
{
	(void)stMsgShell;
	(void)oInMsgHead;
	(void)oInMsgBody;
	(void)data;
	return STATUS_CMD_COMPLETED;
}

E_CMD_STATUS MysqlStep::Callback(util::MysqlAsyncConn *c,util::SqlTask *task,MYSQL_RES *pResultSet)
{
	LOG4_TRACE("%s()",__FUNCTION__);
	if (0 != task->iErrno)
	{
		LOG4_ERROR("%s() mysql error(%d,%s)",__FUNCTION__,task->iErrno,task->errmsg.c_str());
		return Emit(task->iErrno,task->errmsg,task->errmsg);
	}
	m_pMysqlResSet->Init(pResultSet,c->GetMysql());//有无结果集也需要保存
	m_uiTimeOutCounter = 0;//新状态重置超时计数
	return Emit();
}

E_CMD_STATUS MysqlStep::Timeout()
{
	LOG4_WARN("%s() mysql m_strLastCmd(%s)", __FUNCTION__, m_strLastCmd.c_str());
	++m_uiTimeOutCounter;
	if (m_uiTimeOutCounter < m_uiTimeOutMax)
	{
		if (m_uiTimeOutRetry > 0)
		{
			LOG4_WARN("%s() retry. uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u)",
				__FUNCTION__, m_uiTimeOutCounter, m_uiTimeOutMax, m_uiTimeOutRetry);
#ifdef _RUN_CLOCK
			m_RunClock.TotalRunTime();
#endif
			return Emit();
		}
		return STATUS_CMD_RUNNING;
	}
	LOG4_ERROR("%s() uiTimeOutCounter(%u) uiTimeOutMax(%u) uiTimeOutRetry(%u)",
		__FUNCTION__, m_uiTimeOutCounter, m_uiTimeOutMax, m_uiTimeOutRetry);
	return STATUS_CMD_FAULT;
}

bool MysqlStep::Launch(std::unique_ptr<MysqlStep> pStep,uint32 uiTimeOutMax,uint8 uiToRetry,double dTimeout)
{
	if (!pStep)
	{
		LOG4_ERROR("%s() null MysqlStep",__FUNCTION__);
		return(false);
	}
	MysqlStep* pRaw = pStep.get();
	if (pRaw->CurTask().size() == 0)//MysqlStep必须含mysql访问任务
	{
		LOG4_ERROR("%s() CurTask().size() == 0",__FUNCTION__);
		return(false);
	}
	if (!net::Register(std::move(pStep),uiTimeOutMax,uiToRetry,dTimeout))//注册定时任务
	{
		LOG4_ERROR("%s() Register error",__FUNCTION__);
		return(false);
	}
	// pStep moved, pRaw now in mapCallbackStep
	if (!GetLabor()->RegisterCallback(pRaw))//注册mysql访问任务
	{
		LOG4_ERROR("%s() RegisterCallback error",__FUNCTION__);
		if (pRaw->IsRegistered())
		{
			GetLabor()->DeleteCallback(pRaw);
		}
		return(false);
	}
	pRaw->SetStepDesc(std::string("MysqlStep:") + pRaw->m_strLastCmd);

	LOG4_TRACE("%s() uiMysqlStepRegisterCounter:%llu",__FUNCTION__,++uiMysqlStepRegisterCounter);
	return true;
}

//添加新任务(注册过的MysqlStep才能追加)
bool MysqlStep::AppendTask(const std::string &strCmd,uint8 uiCmdType)
{
	if (!IsRegistered())
	{
		return(false);
	}
	LOG4_TRACE("%s()",__FUNCTION__);
	SetTask(strCmd,uiCmdType);
	if (!GetLabor()->RegisterCallback(this))//注册任务
	{
		LOG4_ERROR("%s() RegisterCallback error",__FUNCTION__);
		return(false);
	}
	return true;
}
//添加新任务(注册过的MysqlStep才能追加)
bool MysqlStep::AppendTask(uint8 uiCmdType,const char *fmt,...)
{
	if (!IsRegistered())
	{
		return(false);
	}
	LOG4_TRACE("%s()",__FUNCTION__);
	char printf_buf[1024];//目前最长指令设1024,超过长度的调用AppendTask(const std::string &strCmd,uint8 uiCmdType)
	{
		va_list args;
//		int printed;
		va_start(args, fmt);
		vsnprintf(printf_buf,sizeof(printf_buf),fmt, args);//搜索字符串fmt中需要特定模式的字符(比如%s),args为后面参数的首地址
		va_end(args);
	}
	SetTask(printf_buf,uiCmdType);
	if (!GetLabor()->RegisterCallback(this))//注册任务
	{
		LOG4_ERROR("%s() RegisterCallback error",__FUNCTION__);
		return(false);
	}
	return true;
}

int CustomMysqlHandler::on_execsql(util::MysqlAsyncConn *c, util::SqlTask *task) {
	LOG4_TRACE("%s:%d sql: %s done",__FUNCTION__,__LINE__, task->sql.c_str());
	((net::Worker*)GetLabor())->Dispose(c,task,NULL);
	return 0;
}
int CustomMysqlHandler::on_query(util::MysqlAsyncConn *c, util::SqlTask *task, MYSQL_RES *pResultSet) {
	LOG4_TRACE("%s:%d sql: %s done",__FUNCTION__,__LINE__, task->sql.c_str());
	((net::Worker*)GetLabor())->Dispose(c,task,pResultSet);
	return 0;
}

} /* namespace net */
