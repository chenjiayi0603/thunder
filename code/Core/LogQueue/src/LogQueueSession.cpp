/*
 * LogQueueSession.cpp
 *
 *  Created on: 2017年5月25日
 *      Author: chenjiayi
 */
#include "LogQueueSession.h"

namespace core
{

bool LogQueueSession::Init(const util::CJsonObject& conf,const util::CJsonObject& logTableConf)
{
    if(boInit)
    {
        return true;
    }
    SetCurrentTime();
    {
    	LOAD_CONFIG(conf,"datalog_path",m_FileMgr.m_strDatalogPath);
    	LOAD_CONFIG(conf,"log_queue_num",m_FileMgr.m_uiLogQueueNum);
        {//加载日志表配置
    		LOAD_CONFIG(conf,"logtime_write_interval",m_FileMgr.m_dWriteLogInterval);//写日志时间
    		LOAD_CONFIG(conf,"logtime_create_log_interval",m_FileMgr.m_dCreateLogInterval);//创建日志时间

    		LOAD_CONFIG(conf,"enable_test",m_uiEnableTest);
			LOAD_CONFIG(conf,"test_write_time",m_uiTestWriteTime);
			LOAD_CONFIG(conf,"verify_log",m_uiVerifyLog);
            LOAD_CONFIG(conf,"sync_log",m_FileMgr.m_uiSyncLog);
            LOAD_CONFIG(conf,"log_format",m_FileMgr.m_uiLogFormat);
        }
        {//日志表加载
            util::CJsonObject oLogTables;
            LOAD_CONFIG(logTableConf,"log_tables",oLogTables);
            util::CJsonObject oTable;
            for(int i = 0;i < oLogTables.GetArraySize();++i)
            {
                if (oLogTables.Get(i,oTable))
                {
                	std::string strLogType;int iLogCmd(0);util::CJsonObject head;
                    if (oTable.Get("log_type",strLogType) && oTable.Get("log_cmd",iLogCmd) && oTable.Get("head",head))
                    {
						m_FileMgr.m_oTableHead.insert(std::make_pair(strLogType,head));
						m_FileMgr.m_oTableCmd.insert(std::make_pair(strLogType,iLogCmd));
                    }
                    else
                    {
                        LOG4_WARN("invalid oTable(%s)",oTable.ToString().c_str());
                        return false;
                    }
                }
            }
            if (m_FileMgr.m_oTableHead.size() == 0)
			{
				LOG4_WARN("vecComsumeTables.size() == 0");
				return false;
			}
        }
        m_FileMgr.Init();
    }
    boInit = true;
    TestWriteLogs();
    LOG4_INFO("LogQueueSession init ok");
    return true;
}

net::E_CMD_STATUS LogQueueSession::Timeout()
{
	if (m_FileMgr.GetAllWriteSize() > 0)
	{
		CheckOpenNewLog();
		m_FileMgr.RoutineWrite(false);
	}
	m_FileMgr.CheckSync();
	return net::STATUS_CMD_RUNNING;
}

/*
接口名称：用于采集客户端启动
调用路径：/appTrace?appkey=12345678&eventType=appStartTrace
Appkey为App被分配的appkey
eventType为事件类型
功能说明：用于采集客户端启动
访问方式 ：POST
http://192.168.18.78:18000/appTrace?appkey=12345678910ABC&eventType=appStartTrace

每个消息同步写
TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
net::RunClock TestWriteLogs use time(2561.010010) ms
文件6.68mb
qps:3904.7
吞吐量：2.6 Mb/s

每个消息同步写 + 加合法性检查
TestWriteLogs() TestWriteLogs AppendLog uiSyncLog(1) uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
net::RunClock TestWriteLogs use time(2635.263916) ms
文件6.68mb
qps:3794.6
吞吐量：2.53 Mb/s

每个消息异步写
TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
net::RunClock TestWriteLogs use time(84.639000) ms
文件6.68mb
qps:118147.4
吞吐量：78.9 Mb/s

每个消息异步写 + 加合法性检查
TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
net::RunClock TestWriteLogs use time(184.009995) ms
文件6.68mb
qps:54347.8
吞吐量：36.3 Mb/s

新
每个消息异步写
TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(673) sizeof(LogDataHeader):8 log_info:568 m_uiVerifyLog(0)
 RunClock use time(9.402000) ms
文件6.28mb
约
qps:1000000
吞吐量：628 Mb/s

每个消息异步写 + 加合法性检查
TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(801) sizeof(LogDataHeader):8 log_info:696 m_uiVerifyLog(1)
 RunClock use time(134.341003) ms
文件7.5mb
约
qps:74626
吞吐量：56 Mb/s
 * */
void LogQueueSession::TestWriteLogs()
{
    if (m_uiEnableTest && m_uiTestWriteTime)
    {
    	LOG4_INFO("%s",__FUNCTION__);
    	net::RunClock runClock;
    	runClock.StartClock("TestWriteLogs");
        const std::string strLogInfo = "{\"appkey\":\"1234567890\",\"type\":\"trace\",\"device_id\":\"1234567890\",\"platform\":\"js\",\"session_id\":\"1234567890\",\"event_id\":\"1234567890\",\"eventName\":\"\345\220\257\345\212\250\",\"eventId\":\"1234567890\",\"pageName\":\"\347\231\273\345\275\225\351\241\265\",\"pageUrl\":\"http://192.168.18.44/index.php/API/apidoc/AppStartTrace\",\n\n    \"userName\":\"chenjiayi\",\n    \"location\":\"\346\267\261\345\234\263\",\n    \"gender\":\"\347\224\267\",\n    \"age\":25,\n    \"sourceId\":\"1234567890\",\n    \"sourcePage\":\"\344\275\240\346\210\221\351\207\221\350\236\215\351\246\226\351\241\265\",\n\n    \"deviceId\":\"00-50-56-C0-00-08\",\n    \"ip\":\"192.168.11.232\",\n    \"os\":\"iOS6\",\n    \"deviceBrand\":\"Apple\",\n    \"osVersion\":\"4.0\",\n    \"resolution\":\"640*960\",\n    \"deviceLanguage\":\"zh-CN\",\n    \"modelNumber\":\"iPhone 5s\",\n    \"networkType\":\"WIFI\",\n    \"networkProvider\":\"46000\",\n    \"sysNo\":\"NW_APP\",\n\n    \"startTime\":\"14000000\",\n    \"appVersion\":\"1.0\",\n\n    \"sendTime\":\"14000000\"\n}";
        util::CJsonObject objLog;
        if (!objLog.Parse(strLogInfo))
        {
            LOG4_WARN("%s() failed to parse strLogInfo:%s",__FUNCTION__,strLogInfo.c_str());
            return;
        }
        LOG4_INFO("%s() m_uiTestWriteTime(%u) strLogInfo(%u) objLog:%s",__FUNCTION__,m_uiTestWriteTime,strLogInfo.size(),objLog.ToString().c_str());
        logqueue::log oInAsk;
        oInAsk.set_type("trace");
        oInAsk.set_log_info(objLog.ToString());//不含回车
        uint32 uiCounter(0);
        for(;uiCounter < m_uiTestWriteTime;++uiCounter)
        {
            int nErrCode = AppendLog(oInAsk);
            if (nErrCode)
            {
                LOG4_WARN("%s() failed to AppendLog nErrCode(%d),uiCounter(%u)",__FUNCTION__,nErrCode,uiCounter);
                break;
            }
        }
        runClock.EndClock();
        LOG4_INFO("%s() use time(%lf) uiCounter(%u) strLogInfo(%u) log_info:%u",__FUNCTION__,runClock.LastUseTime(),uiCounter,strLogInfo.size(),oInAsk.log_info().size());
        /*
         78.39mb 100000 4516ms 验证
         78.39mb 100000 3724ms 不验证
         783.92mb 1000000 40792ms 不验证
         1	100000/1000000	1进程写10w/100w日志，每次1w，吞吐量21/19(MB/s)/,26852/24514qps(日志每秒)


         5*783.92mb=3919.6mb 1000000*5=5000000 44849ms 不验证
         5	5000000		5进程写总共500w日志，每次1w，吞吐量87.39(MB/s)/,111485qps(日志每秒)

		 10*783.92mb=7839.2mb 10000000 80611ms 不验证
         10	10000000		10进程写总共1000w日志，每次1w，吞吐量97.24(MB/s)/,124052qps(日志每秒)
         * */
    }
    m_uiEnableTest = 0;
}

bool LogQueueSession::CheckOpenNewLog()
{
    //每隔一段时间创建一个新的日志文件
    if(m_FileMgr.NeedNewLogCycle(SetCurrentTime()))
    {
        LOG4_TRACE("%s() m_dCreateLogInterval(%lf) m_uiCreateLogLastTime(%u) m_uiCurrentTime(%u)",
                __FUNCTION__,m_FileMgr.m_dCreateLogInterval,m_FileMgr.m_uiCreateLogLastTime,m_uiCurrentTime);
        return m_FileMgr.OpenLog();
    }
    return true;
}

int LogQueueSession::AppendLog(logqueue::log &message)
{
	if (m_uiVerifyLog)
	{
		int nErrCode = VerifyLog(message);
		if (nErrCode)
		{
			LOG4_WARN("failed to VerifyLog");
			return nErrCode;
		}
	}
    if (!m_FileMgr.AddLog(message))
    {
    	return ERR_SERVER_LOGIC_ERROR;
    }
    return ERR_OK;
}

int LogQueueSession::VerifyLog(logqueue::log &message)
{
	if (message.log_info().size() == 0|| message.type().size() == 0)
	{
		LOG4_WARN("%s() log_info().size() == 0 || type().size() == 0",__FUNCTION__);
		return ERR_INVALID_PARAMS;
	}
	if (m_FileMgr.m_oTableHead.end() == m_FileMgr.m_oTableHead.find(message.type()))
	{
		LOG4_WARN("%s() log_type(%s) invalid",__FUNCTION__,message.type().c_str());
		return ERR_INVALID_PARAMS;
	}
	util::CJsonObject oLogMsg;
	if (!oLogMsg.Parse(message.log_info()))
	{
		LOG4_WARN("%s() invalid data.failed to parse log_info:%s",__FUNCTION__,message.log_info().c_str());
		return ERR_INVALID_PARAMS;
	}
	return ERR_OK;
}


LogQueueSession* GetLogQueueSession()
{
    LogQueueSession* pSess = (LogQueueSession*) net::GetSession(LOGQUEUE_SESSIN_ID,"analysis::LogQueueSession");
    if (pSess)
    {
        return (pSess);
    }
    pSess = new LogQueueSession();
    if (pSess == NULL)
    {
        LOG4_ERROR("error %d: new LogQueueSession() error!",net::ERR_NEW);
        return (NULL);
    }
    util::CJsonObject oCurrentConf;       ///< 当前加载的配置
	if (!net::GetJsonConfigFile(net::GetConfigPath() + std::string("QueueCmd.json"),oCurrentConf))
	{
		delete pSess;
		pSess = NULL;
		return (NULL);
	}
    util::CJsonObject oLogTablesConf;
	if (!net::GetJsonConfigFile(net::GetConfigPath() + std::string("QueueTables.json"),oLogTablesConf))
	{
		delete pSess;
		pSess = NULL;
		return (NULL);
	}
    if (net::RegisterCallback(pSess))
    {
        if (!pSess->Init(oCurrentConf,oLogTablesConf))
        {
        	net::DeleteCallback(pSess);
            LOG4_ERROR("LogQueueSession init error!");
            return (NULL);
        }
        LOG4_DEBUG("register LogQueueSession ok!");
        return (pSess);
    }
    else
    {
        LOG4_ERROR("register LogQueueSession error!");
        delete pSess;
        pSess = NULL;
    }
    return (NULL);
}


}
;
//namespace core
