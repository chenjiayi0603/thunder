/*******************************************************************************
 * Project:  Net
 * @file     MysqlStep.hpp
 * @brief    带mysql的异步步骤基类
 * @author   cjy
 * @date:    2017年8月15日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_MYSQLSTEP_HPP_
#define SRC_STEP_MYSQLSTEP_HPP_
#include <set>
#include <list>
#include <memory>
#include "dbi/MysqlAsyncConn.h"
#include "Step.hpp"

namespace net
{
class CustomMysqlHandler;
class Worker;

/**
 * @brief Mysql访问步骤
 * @note 异步提交 SQL，结果在 Callback(MysqlAsyncConn*, ...) 中返回；超时与重试由 MysqlStep 自身字段控制。
 */
class MysqlStep: public Step
{
public:
	MysqlStep(const std::string& strHost, int iPort,const std::string& dbname,const std::string& user,
			const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut=3);
	explicit MysqlStep(const util::tagDbConnInfo &dbConnInfo);
	MysqlStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead,const util::tagDbConnInfo &dbConnInfo);
	MysqlStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead,const std::string& strHost, int iPort,const std::string& dbname,
			const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut=3);
	MysqlStep(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg,const std::string& strHost, int iPort,const std::string& dbname,
			const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut=3);
	MysqlStep(const tagMsgShell& stReqMsgShell, const HttpMsg& oInHttpMsg,const util::tagDbConnInfo &dbConnInfo);
	void Init(const std::string& strHost, int iPort,const std::string& dbname,
			const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut=3);
	/** @brief 由 Register 设置步骤级超时次数与是否超时重试 */
	void Init(uint32 uiTimeOutMax, uint8 uiTimeOutRetry)
	{
		m_uiTimeOutMax = uiTimeOutMax;
		m_uiTimeOutRetry = uiTimeOutRetry;
	}
	void SetStepDesc(const std::string& s) { m_strStepDesc = s; }

	virtual ~MysqlStep();
	/**
	 * @brief 设置配置
	 */
	void SetConf(const util::tagDbConnInfo &dbConnInfo);
	void SetConf(const std::string& strHost, int iPort,const std::string& dbname,
			const std::string& user,const std::string& passwd,const std::string &dbcharacterset,uint32 uiTimeOut=3);
	/**
	 * @brief 开始步骤
	 */
	static bool Launch(std::unique_ptr<MysqlStep> pStep,uint32 uiTimeOutMax=3,uint8 uiToRetry=1,double dTimeout=3.0);//
	/**
	* @brief 追加mysql访问任务
	* @note (注册过的MysqlStep才能追加).追加mysql访问任务后，会异步提交访问并返回结果
	*/
	bool AppendTask(const std::string &strCmd,uint8 uiCmdType);
	bool AppendTask(uint8 uiCmdType,const char *fmt,...);
    /**
     * @brief Mysql步骤回调
     * @param c Mysql连接上下文
     * @param status 回调状态
     * @param pResultSet 执行结果集
     */
    virtual E_CMD_STATUS Callback(util::MysqlAsyncConn *c,util::SqlTask *task,MYSQL_RES *pResultSet);
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,void* data = nullptr) override;
    /**
	 * @brief 执行步骤
	 */
    E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "") override;
    /**
     * @brief 超时回调
     * @return 回调状态
     */
    virtual E_CMD_STATUS Timeout()override;
    /**
	* @brief 设置mysql访问任务
	* @note (注册过的MysqlStep才能追加).追加mysql访问任务后，会异步提交访问并返回结果
	*/
    void SetTask(const std::string &strCmd,uint8 uiCmdType)
    {
    	LOG4_TRACE("%s() Cmd(%s)",__FUNCTION__,strCmd.c_str());
    	m_strCmd = strCmd;
    	m_uiCmdType = uiCmdType;
    	if (util::eSqlTaskOper_exec == uiCmdType)
    	{
    		m_uiTimeOutRetry = 0;//访问mysql的写操作不重发
    	}
    }
    /**
	* @brief 设置mysql访问任务
	*/
    int SetTask(uint8 uiCmdType,const char *fmt,...)
    {
    	m_uiCmdType = uiCmdType;
		char printf_buf[1024];//目前最长指令设1024,超过长度的调用SetTask(const std::string &strCmd,uint8 uiCmdType)
		va_list args;
		int printed;
		va_start(args, fmt);
		printed = vsnprintf(printf_buf,sizeof(printf_buf),fmt, args);//搜索字符串fmt中需要特定模式的字符(比如%s),args为后面参数的首地址
		va_end(args);
		m_strCmd = printf_buf;
		if (util::eSqlTaskOper_exec == uiCmdType)
		{
			m_uiTimeOutRetry = 0;//访问mysql的写操作不重发
		}
		LOG4_TRACE("%s() Cmd(%s)",__FUNCTION__,m_strCmd.c_str());
		m_strLastCmd = m_strCmd;
		return printed;
    }
    void ClearTask(){m_strCmd.clear();m_uiCmdType=0;}
    const std::string& CurTask()const {return m_strCmd;}
    //参数
    std::string m_strHost;
    int m_iPort = 0;
    std::string m_strDbName;
    std::string m_strUser;
    std::string m_strPasswd;
    std::string m_dbcharacterset;
    unsigned int m_uiTimeOut = 0;

    //任务
    std::string m_strCmd;
    std::string m_strLastCmd;
    uint8 m_uiCmdType = 0;
	//回调结果
	std::unique_ptr<util::MysqlResSet> m_pMysqlResSet;

	int m_iErrno = 0;
	std::string m_strStepDesc = "MysqlStep";
	std::string m_strErrMsg;
protected:
	uint32 m_uiTimeOutCounter = 0;
	uint32 m_uiTimeOutMax = 3;
	uint8 m_uiTimeOutRetry = 0;
};

class CustomMysqlHandler: public util::MysqlHandler {
public:
	explicit CustomMysqlHandler(MysqlStep* step):m_uiMysqlStepSeq(step->GetSequence()){}
	virtual ~CustomMysqlHandler() = default;
	int on_execsql(util::MysqlAsyncConn *c, util::SqlTask *task);
	int on_query(util::MysqlAsyncConn *c, util::SqlTask *task, MYSQL_RES *pResultSet);
	uint32 m_uiMysqlStepSeq;
};

} /* namespace net */

#endif /* SRC_STEP_REDISSTEP_HPP_ */
