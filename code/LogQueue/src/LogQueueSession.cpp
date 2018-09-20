/*
 * LogQueueSession.cpp
 *
 *  Created on: 2017年5月25日
 *      Author: chenjiayi
 */
#include "LogQueueSession.h"

namespace analysis
{

bool LogQueueSession::Init(const util::CJsonObject& conf,const util::CJsonObject& logTableConf)
{
    if(boInit)
    {
        return true;
    }
    SetCurrentTime();
    {
        if (!conf.Get("datalog_path", m_FileMgr.m_strDatalogPath))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load LogQueueSession conf datalog_path");
            return false;
        }
        if (!conf.Get("log_queue_num", m_FileMgr.m_uiLogQueueNum))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load LogQueueSession conf log_queue_num");
            return false;
        }
        {//加载日志表配置
        	conf.Get("verify_log",m_uiVerifyLog);

            if (!conf.Get("logtime_write_interval", m_FileMgr.m_dWriteLogInterval)) //写日志时间
            {
                LOG4CPLUS_ERROR_FMT(GetLogger(),"error %d: new LogSession() get logtime_write_interval failed!");
                return false;
            }
            if (!conf.Get("logtime_create_log_interval",m_FileMgr.m_dCreateLogInterval)) //创建日志时间
            {
                LOG4CPLUS_ERROR_FMT(GetLogger(),"error %d: new LogSession() get logtime_create_log_interval failed!");
                return false;
            }
            conf.Get("test_write_time",m_uiTestWriteTime);
            conf.Get("sync_log",m_FileMgr.m_uiSyncLog);
            conf.Get("log_format",m_FileMgr.m_uiLogFormat);
        }
        {//日志表加载
        	std::vector<util::CJsonObject>& vecComsumeTables = m_FileMgr.m_vecComsumeTables;
            util::CJsonObject oLogTables;
            if (!logTableConf.Get("log_tables",oLogTables))
            {
                LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load LogQueueSession conf log_list");
                return false;
            }
            util::CJsonObject oTable;
            for(int i = 0;i < oLogTables.GetArraySize();++i)
            {
                uint32 nLogCmd(0);
                std::string strLogType;
                if (oLogTables.Get(i,oTable))
                {
                    if (oTable.Get("log_cmd",nLogCmd) && oTable.Get("log_type",strLogType))
                    {
                        LogTable oLogTable;
                        oLogTable.nLogCmd = nLogCmd;
                        oLogTable.strLogType = strLogType;
                        util::CJsonObject fields;
                        std::vector<std::string> fieldsVec;
                        if (oTable.Get("fields",fields))
                        {
                            for(int i = 0;i < fields.GetArraySize();++i)
                            {
                                std::string strField;
                                if (fields.Get(i,strField))
                                {
                                    fieldsVec.push_back(strField);
                                }
                            }
                            LOG4CPLUS_INFO_FMT(GetLogger(),"%s() nLogCmd(%u) strLogType(%s) fieldsVec size(%u) fields(%s)",
                                            __FUNCTION__,nLogCmd,strLogType.c_str(),fieldsVec.size(),
                                            fields.ToFormattedString().c_str());
                        }
                        oLogTable.fieldsVec = fieldsVec;//如果有必须字段的则进行字段校验
                        m_mapLogTable.insert(std::make_pair(strLogType,oLogTable));
                        util::CJsonObject comsume_head;
                        if (oTable.Get("comsume_head",comsume_head))
                        {
                            if (vecComsumeTables.size() <= nLogCmd)
                            {
                            	vecComsumeTables.resize(nLogCmd + 1);
                            }
                            vecComsumeTables[nLogCmd] = comsume_head;
                            LOG4_INFO("%s() vecComsumeTables.size(%u)",__FUNCTION__,vecComsumeTables.size());
                        }
                    }
                    else
                    {
                        LOG4CPLUS_WARN_FMT(GetLogger(),"invalid oTable(%s)",oTable.ToString().c_str());
                    }
                }
                if (vecComsumeTables.size() == 0)
                {
                    LOG4CPLUS_WARN_FMT(GetLogger(),"vecComsumeTables.size() == 0");
                    return false;
                }
            }
        }
        m_FileMgr.Init(GetLabor(),GetLogger(),m_strWorkerIdentify);
    }
    boInit = true;
    TestWriteLogs();
    LOG4CPLUS_INFO_FMT(GetLogger(),"LogQueueSession init ok");
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
	LOG4_INFO("%s",__FUNCTION__);
    if (m_uiTestWriteTime > 0)
    {
    	net::RunClock runClock;
    	runClock.StartClock("TestWriteLogs",GetLogger());
        const std::string strLogInfo = "{\"appkey\":\"1234567890\",\"type\":\"trace\",\"device_id\":\"1234567890\",\"platform\":\"js\",\"session_id\":\"1234567890\",\"event_id\":\"1234567890\",\"eventName\":\"\345\220\257\345\212\250\",\"eventId\":\"1234567890\",\"pageName\":\"\347\231\273\345\275\225\351\241\265\",\"pageUrl\":\"http://192.168.18.44/index.php/API/apidoc/AppStartTrace\",\n\n    \"userName\":\"chenjiayi\",\n    \"location\":\"\346\267\261\345\234\263\",\n    \"gender\":\"\347\224\267\",\n    \"age\":25,\n    \"sourceId\":\"1234567890\",\n    \"sourcePage\":\"\344\275\240\346\210\221\351\207\221\350\236\215\351\246\226\351\241\265\",\n\n    \"deviceId\":\"00-50-56-C0-00-08\",\n    \"ip\":\"192.168.11.232\",\n    \"os\":\"iOS6\",\n    \"deviceBrand\":\"Apple\",\n    \"osVersion\":\"4.0\",\n    \"resolution\":\"640*960\",\n    \"deviceLanguage\":\"zh-CN\",\n    \"modelNumber\":\"iPhone 5s\",\n    \"networkType\":\"WIFI\",\n    \"networkProvider\":\"46000\",\n    \"sysNo\":\"NW_APP\",\n\n    \"startTime\":\"14000000\",\n    \"appVersion\":\"1.0\",\n\n    \"sendTime\":\"14000000\"\n}";
        util::CJsonObject objLog;
        if (!objLog.Parse(strLogInfo))
        {
            LOG4_WARN("%s() failed to parse strLogInfo:%s",__FUNCTION__,strLogInfo.c_str());
            return;
        }
        LOG4_INFO("%s() TestWriteLogs AppendLog m_uiTestWriteTime(%u) strLogInfo.size(%u) sizeof(LogDataHeader):%u objLog:%s",
                                __FUNCTION__,m_uiTestWriteTime,strLogInfo.size(),sizeof(LogDataHeader),objLog.ToString().c_str());
        behaviour::behaviour oInAsk;
        oInAsk.set_log_cmd(1);
        oInAsk.set_type("trace");
        oInAsk.set_log_info(objLog.ToString());//不含回车
        uint32 uiCounter(0);
        for(;uiCounter < m_uiTestWriteTime;++uiCounter)
        {
            int nErrCode(0);
            if (!AppendLog(oInAsk,nErrCode))
            {
                LOG4_WARN("%s() failed to AppendLog nErrCode(%d),uiCounter(%u)",__FUNCTION__,nErrCode,uiCounter);
                break;
            }
        }
        LOG4_INFO("%s() TestWriteLogs AppendLog uiCounter(%u) strLogInfo.size(%u) sizeof(LogDataHeader):%u log_info:%u m_uiVerifyLog(%u)",
                        __FUNCTION__,uiCounter,strLogInfo.size(),sizeof(LogDataHeader),oInAsk.log_info().size(),m_uiVerifyLog);
        runClock.EndClock();
    }
    m_uiTestWriteTime = 0;
}

bool LogQueueSession::CheckOpenNewLog()
{
    //每隔一段时间创建一个新的日志文件
    if(m_FileMgr.NeedNewLogCycle(SetCurrentTime()))
    {
        LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() m_dCreateLogInterval(%lf) m_uiCreateLogLastTime(%u) m_uiCurrentTime(%u)",
                __FUNCTION__,m_FileMgr.m_dCreateLogInterval,m_FileMgr.m_uiCreateLogLastTime,m_uiCurrentTime);
        return m_FileMgr.OpenLog();
    }
    return true;
}

bool LogQueueSession::AppendLog(behaviour::behaviour &message,int& nErrCode)
{
    if (!VerifyLog(message,nErrCode))
    {
    	LOG4CPLUS_WARN_FMT(GetLogger(),"failed to VerifyLog");
    	return false;
    }
    if (!m_FileMgr.AddLog(message))
    {
    	nErrCode = ERR_SERVER_LOGIC_ERROR;
    	return false;
    }
    nErrCode = ERR_RESPONSE_OK;
    return true;
}

bool LogQueueSession::VerifyLog(behaviour::behaviour &message,int& nErrCode)
{
	if (message.log_info().size() == 0)
	{
		LOG4CPLUS_WARN_FMT(GetLogger(), "%s() message.log_info().size() == 0",__FUNCTION__);
		nErrCode = ERR_INVALID_PARAMS;
		return false;
	}
	if (m_uiVerifyLog > 0)
	{//校验字段合法性
		std::map<std::string,LogTable>::const_iterator cit = m_mapLogTable.find(message.type());
		if (m_mapLogTable.end() == cit)
		{
			LOG4CPLUS_WARN_FMT(GetLogger(), "%s() log_type(%s) invalid",__FUNCTION__,message.type().c_str());
			nErrCode = ERR_INVALID_PARAMS;
			return false;
		}
		const LogTable& oLogTable = cit->second;
		message.set_log_cmd(oLogTable.nLogCmd);
		m_jsonLogMsg.Clear();
		if (!m_jsonLogMsg.Parse(message.log_info()))
		{
			LOG4CPLUS_WARN_FMT(GetLogger(), "%s() invalid data.failed to parse log_info:%s",
							__FUNCTION__,message.log_info().c_str());
			nErrCode = ERR_INVALID_PARAMS;
			return false;
		}
		if (m_jsonLogMsg.GetArraySize() > 0)
		{
			int nArraySize = m_jsonLogMsg.GetArraySize();
			for(int i = 0;i < nArraySize;++i)
			{
				std::vector<std::string>::const_iterator citField = oLogTable.fieldsVec.begin();
				std::vector<std::string>::const_iterator citFieldEnd = oLogTable.fieldsVec.end();
				for(;citField != citFieldEnd;++citField)
				{
					if(m_jsonLogMsg[i](*citField).size() == 0)
					{
						LOG4CPLUS_WARN_FMT(GetLogger(), "%s() invalid data.failed to get field:%s,log_info:%s",
										__FUNCTION__,citField->c_str(),message.log_info().c_str());
						nErrCode = ERR_INVALID_PARAMS;
						return false;
					}
				}
			}
		}
		else
		{
			std::vector<std::string>::const_iterator citField = oLogTable.fieldsVec.begin();
			std::vector<std::string>::const_iterator citFieldEnd = oLogTable.fieldsVec.end();
			for(;citField != citFieldEnd;++citField)
			{
				if(m_jsonLogMsg(*citField).size() == 0)
				{
					LOG4CPLUS_WARN_FMT(GetLogger(), "%s() invalid data.failed to get field:%s,log_info:%s",
									__FUNCTION__,citField->c_str(),message.log_info().c_str());
					nErrCode = ERR_INVALID_PARAMS;
					return false;
				}
			}
		}
		//LOG4CPLUS_DEBUG_FMT(GetLogger(), "%s() succ to validate log_info:%s",__FUNCTION__,message.log_info().c_str());
	}
	if (message.log_cmd() == 0)
	{
		LOG4CPLUS_WARN_FMT(GetLogger(), "%s() message.log_cmd() == 0,message:%s",__FUNCTION__,message.DebugString().c_str());
		nErrCode = ERR_INVALID_PARAMS;
		return false;
	}
	return true;
}


LogQueueSession* GetLogQueueSession(net::Labor* pLabor,const std::string &strConfigPath,const std::string& strWorkerIdentify)
{
    LogQueueSession* pSess = (LogQueueSession*) pLabor->GetSession(LOGQUEUE_SESSIN_ID,"analysis::LogQueueSession");
    if (pSess)
    {
        return (pSess);
    }
    pSess = new LogQueueSession();
    if (pSess == NULL)
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "error %d: new LogQueueSession() error!",
                        net::ERR_NEW);
        return (NULL);
    }
    util::CJsonObject   oCurrentConf;       ///< 当前加载的配置
    {
        //配置文件路径查找
        std::string strConfFile = strConfigPath + std::string("/QueueCmd.json");
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(), "CONF FILE = %s.", strConfFile.c_str());

        std::ifstream fin(strConfFile.c_str());
        //配置信息输入流
        if (fin.good())
        {
            //解析配置信息 JSON格式
            std::stringstream ssContent;
            ssContent << fin.rdbuf();
            if (!oCurrentConf.Parse(ssContent.str()))
            {
                //配置文件解析失败
                LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),
                                "Read conf (%s) error,it's maybe not a json file!",
                                strConfFile.c_str());
                ssContent.str("");
                fin.close();
                delete pSess;
                pSess = NULL;
                return (NULL);
            }
        }
        else
        {
            //配置信息流读取失败
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "Open conf (%s) error!",
                            strConfFile.c_str());
            delete pSess;
            pSess = NULL;
            return (NULL);
        }
        pSess->m_strConfigPath = strConfigPath;
        pSess->m_strWorkerIdentify = strWorkerIdentify;
    }
    util::CJsonObject   oLogTablesConf;
    {
        std::string strConfFile = strConfigPath + std::string("/QueueTables.json");
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(), "CONF FILE = %s.", strConfFile.c_str());
        std::ifstream fin(strConfFile.c_str());
        if (fin.good())
        {
            std::stringstream ssContent;
            ssContent << fin.rdbuf();
            if (!oLogTablesConf.Parse(ssContent.str()))
            {
                LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),
                                "Read conf (%s) error,it's maybe not a json file!",
                                strConfFile.c_str());
                ssContent.str("");
                fin.close();
                delete pSess;
                pSess = NULL;
                return (NULL);
            }
        }
        else
        {
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "Open conf (%s) error!",
                            strConfFile.c_str());
            delete pSess;
            pSess = NULL;
            return (NULL);
        }
    }
    if (pLabor->RegisterCallback(pSess))
    {
        if (!pSess->Init(oCurrentConf,oLogTablesConf))
        {
            pLabor->DeleteCallback(pSess);
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "LogQueueSession init error!");
            return (NULL);
        }
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(), "register LogQueueSession ok!");
        return (pSess);
    }
    else
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "register LogQueueSession error!");
        delete pSess;
        pSess = NULL;
    }
    return (NULL);
}


}
;
//namespace analysis
