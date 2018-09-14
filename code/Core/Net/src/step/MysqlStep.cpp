/*******************************************************************************
 * Project:  Net
 * @file     RedisStep.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年8月15日
 * @note
 * Modify history:
 ******************************************************************************/
#include "MysqlStep.hpp"
#include "labor/duty/Worker.hpp"

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
:StepState(stReqMsgShell,oReqMsgHead)
{
	Init(dbConnInfo.m_szDbHost,dbConnInfo.m_uiDbPort,dbConnInfo.m_szDbName,
			dbConnInfo.m_szDbUser,dbConnInfo.m_szDbPwd,dbConnInfo.m_szDbCharSet,dbConnInfo.uiTimeOut);
}
MysqlStep::MysqlStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead,const std::string& strHost, int iPort,const std::string& dbname,
		const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut)
:StepState(stReqMsgShell,oReqMsgHead)
{
	Init(strHost,iPort,dbname,user,passwd,dbcharacterset,uiTimeOut);
}
MysqlStep::MysqlStep(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg,const std::string& strHost, int iPort,const std::string& dbname,
		const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut)
:StepState(stReqMsgShell,oInHttpMsg)
{
	Init(strHost,iPort,dbname,user,passwd,dbcharacterset,uiTimeOut);
}
MysqlStep::MysqlStep(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg,const util::tagDbConnInfo &dbConnInfo)
:StepState(stReqMsgShell,oInHttpMsg)
{
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

	snprintf(m_dbConnInfo.m_szDbHost,sizeof(m_dbConnInfo.m_szDbHost),strHost.c_str());
	m_dbConnInfo.m_uiDbPort = iPort;
	snprintf(m_dbConnInfo.m_szDbName,sizeof(m_dbConnInfo.m_szDbName),dbname.c_str());
	snprintf(m_dbConnInfo.m_szDbUser,sizeof(m_dbConnInfo.m_szDbUser),user.c_str());
	snprintf(m_dbConnInfo.m_szDbPwd,sizeof(m_dbConnInfo.m_szDbPwd),passwd.c_str());
	snprintf(m_dbConnInfo.m_szDbCharSet,sizeof(m_dbConnInfo.m_szDbCharSet),dbcharacterset.c_str());
	m_dbConnInfo.uiTimeOut = uiTimeOut;

	m_uiCmdType = 0;
	m_pMysqlResSet = new util::MysqlResSet();
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
	if (m_pMysqlResSet)
	{
		delete m_pMysqlResSet;
		m_pMysqlResSet = NULL;
	}
}

E_CMD_STATUS MysqlStep::Callback(util::MysqlAsyncConn *c,util::SqlTask *task,MYSQL_RES *pResultSet)
{
	LOG4CPLUS_TRACE_FMT(GetLogger(),"%s()",__FUNCTION__);
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
    LOG4_WARN("%s() mysql m_strLastCmd(%s)",__FUNCTION__,m_strLastCmd.c_str());
    return StepState::Timeout();
}

bool MysqlStep::Launch(Labor* pLabor,MysqlStep *pStep,uint32 uiTimeOutMax,uint8 uiToRetry,double dTimeout)
{
	if (pStep == NULL)
	{
		LOG4_ERROR_S(pLabor,"%s() null MysqlStep",__FUNCTION__);
		return(false);
	}
	if (pStep->CurTask().size() == 0)//MysqlStep必须含mysql访问任务
	{
		LOG4_ERROR_S(pLabor,"%s() CurTask().size() == 0",__FUNCTION__);
		return(false);
	}
	if (!pStep->IsRegistered())
	{
		if (!StepState::Register(pLabor,pStep,uiTimeOutMax,uiToRetry,dTimeout))//注册定时任务
		{
			LOG4_ERROR_S(pLabor,"%s() StepState::Register error",__FUNCTION__);
			return(false);
		}
	}
	if (!pLabor->RegisterCallback(pStep))//注册mysql访问任务
	{
		LOG4_ERROR_S(pLabor,"%s() RegisterCallback error",__FUNCTION__);
		delete pStep;
		pStep = NULL;
		return(false);
	}
	pStep->SetStepDesc(std::string("MysqlStep:") + pStep->m_strLastCmd);
	LOG4_TRACE_S(pLabor,"%s() uiMysqlStepRegisterCounter:%llu StageClock(%d,%d)",__FUNCTION__,++uiMysqlStepRegisterCounter,
	                pStep->m_StageClock.boInit,pStep->m_StageClock.boStart);
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
	if (!RegisterCallback(this))//注册任务
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
		int printed;
		va_start(args, fmt);
		printed = vsnprintf(printf_buf,sizeof(printf_buf),fmt, args);//搜索字符串fmt中需要特定模式的字符(比如%s),args为后面参数的首地址
		va_end(args);
	}
	SetTask(printf_buf,uiCmdType);
	if (!RegisterCallback(this))//注册任务
	{
		LOG4_ERROR("%s() RegisterCallback error",__FUNCTION__);
		return(false);
	}
	return true;
}

static uint64 uiExecTaskCounter = 0;
int CustomMysqlHandler::on_execsql(util::MysqlAsyncConn *c, util::SqlTask *task) {
	++uiExecTaskCounter;
	LOG4_TRACE_S(((CustomMysqlHandler*) task->handler)->m_pNodeWorker,"%s:%d sql: %s done uiExecTaskCounter:%llu",
			__FUNCTION__,__LINE__, task->sql.c_str(),uiExecTaskCounter);
	((CustomMysqlHandler*) task->handler)->m_pNodeWorker->Dispose(c,task,NULL);
	return 0;
}
int CustomMysqlHandler::on_query(util::MysqlAsyncConn *c, util::SqlTask *task, MYSQL_RES *pResultSet) {
	++uiExecTaskCounter;
	LOG4_TRACE_S(((CustomMysqlHandler*) task->handler)->m_pNodeWorker,"%s:%d sql: %s done uiExecTaskCounter:%llu",
			__FUNCTION__,__LINE__, task->sql.c_str(),uiExecTaskCounter);
	((CustomMysqlHandler*) task->handler)->m_pNodeWorker->Dispose(c,task,pResultSet);
	return 0;
}

bool StateSendToMysql(MysqlStep* state)
{
	STAGE_TEST_PARAM_LOG(SendToMysqlParam,state,"StateSendToMysql");
	state->SetTask(pStageParam->m_strSql,pStageParam->m_uiCmdType);
	return state->GetLabor()->RegisterCallback(state);
}

bool StateSendToMysqlCallback(MysqlStep* state)
{
	SendToMysqlParam* pParam = (SendToMysqlParam*)state->GetData();
	if (!pParam)
	{
		LOG4_WARN_S(state,"StateSendToMsgShellCallback pParam null");
		return false;
	}
	LOG4_TRACE_S(state,"StateSendToMsgShellCallback ok");
	return true;
}

} /* namespace net */
