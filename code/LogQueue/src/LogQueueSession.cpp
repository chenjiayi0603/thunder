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
        if (!conf.Get("datalog_path", m_datalogPath))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load LogQueueSession conf datalog_path");
            return false;
        }
        if (!conf.Get("log_queue_num", m_logQueueNum))
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load LogQueueSession conf log_queue_num");
            return false;
        }
        {//加载日志表配置
            if (!conf.Get("logtime_write_interval", m_writeLogInterval)) //写日志时间
            {
                LOG4CPLUS_ERROR_FMT(GetLogger(),
                                "error %d: new LogSession() get logtime_write_interval failed!");
                return false;
            }
            if (!conf.Get("logtime_create_log_interval",
                            m_createLogInterval)) //创建日志时间
            {
                LOG4CPLUS_ERROR_FMT(GetLogger(),
                                "error %d: new LogSession() get logtime_create_log_interval failed!");
                return false;
            }
            conf.Get("test_write_time",m_uiTestWriteTime);
            conf.Get("verify_log",m_uiVerifyLog);
            conf.Get("sync_log",m_uiSyncLog);
            conf.Get("log_format",m_uiLogFormat);
        }
        {//日志表加载
            util::CJsonObject oLogTables;
            if (!logTableConf.Get("log_tables",oLogTables))
            {
                LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to load LogQueueSession conf log_list");
                return false;
            }
            util::CJsonObject oTable;
            for(int i = 0;i < oLogTables.GetArraySize();++i)
            {
                /*
                {
                    "log_cmd":1,
                    "log_type":"startTrace",
                    "fields": [
                         "event_id",
                         "session_id",
                         "user_id",
                         "device_id"
                    ],
                    "comsume_head":{"index":{"_index":"db_start_trace","_type":"tb_start_trace"}}
                }
                 * */
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
                        m_mapLogTypes.insert(std::make_pair(strLogType,oLogTable));
                        util::CJsonObject comsume_head;
                        if (oTable.Get("comsume_head",comsume_head))
                        {
                            if (m_vecComsumeTables.size() <= nLogCmd)
                            {
                                m_vecComsumeTables.resize(nLogCmd + 1);
                            }
                            m_vecComsumeTables[nLogCmd] = comsume_head;
                            LOG4_INFO("%s() m_vecComsumeTables.size(%u)",__FUNCTION__,m_vecComsumeTables.size());
                        }
                    }
                    else
                    {
                        LOG4CPLUS_WARN_FMT(GetLogger(),"invalid oTable(%s)",oTable.ToString().c_str());
                    }
                }
                if (m_vecComsumeTables.size() == 0)
                {
                    LOG4CPLUS_WARN_FMT(GetLogger(),"m_vecComsumeTables.size() == 0");
                    return false;
                }
            }
        }
        m_localFileMgr.SetLabor(GetLabor());
        m_localFileMgr.SetLogger(GetLogger());
        m_localFileMgr.SetDatalogPath(m_datalogPath);
        m_localFileMgr.SetWorkerIdentify(m_strWorkerIdentify);
        m_localFileMgr.SetSyncLog(m_uiSyncLog);
        m_localFileMgr.SetLogQueueNum(m_logQueueNum);
        m_localFileMgr.SetLogFormat(m_uiLogFormat);
        m_localFileMgr.m_vecComsumeTables = m_vecComsumeTables;
    }
    boInit = true;
    return true;
}

void LogQueueSession::TestWriteLogs()
{
    if (boTestWriteLogs)
    {
        return;
    }
    boTestWriteLogs = true;
    if (m_uiTestWriteTime > 0)
    {
        /*
                                        接口名称：用于采集客户端启动
                                        调用路径：/appTrace?appkey=12345678&eventType=appStartTrace
                Appkey为App被分配的appkey
                eventType为事件类型
                                        功能说明：用于采集客户端启动
                                        访问方式 ：POST
            http://192.168.18.78:18000/appTrace?appkey=12345678910ABC&eventType=appStartTrace
         * */
        m_CustomClock.Start("TestWriteLogs",GetLogger());
        const std::string strLogInfo = "{\"eventName\":\"\345\220\257\345\212\250\",\"eventId\":\"1234567890\",\"pageName\":\"\347\231\273\345\275\225\351\241\265\",\"pageUrl\":\"http://192.168.18.44/index.php/API/apidoc/AppStartTrace\",\n\n    \"userName\":\"chenjiayi\",\n    \"location\":\"\346\267\261\345\234\263\",\n    \"gender\":\"\347\224\267\",\n    \"age\":25,\n    \"sourceId\":\"1234567890\",\n    \"sourcePage\":\"\344\275\240\346\210\221\351\207\221\350\236\215\351\246\226\351\241\265\",\n\n    \"deviceId\":\"00-50-56-C0-00-08\",\n    \"ip\":\"192.168.11.232\",\n    \"os\":\"iOS6\",\n    \"deviceBrand\":\"Apple\",\n    \"osVersion\":\"4.0\",\n    \"resolution\":\"640*960\",\n    \"deviceLanguage\":\"zh-CN\",\n    \"modelNumber\":\"iPhone 5s\",\n    \"networkType\":\"WIFI\",\n    \"networkProvider\":\"46000\",\n    \"sysNo\":\"NW_APP\",\n\n    \"startTime\":\"14000000\",\n    \"appVersion\":\"1.0\",\n\n    \"sendTime\":\"14000000\"\n}";
        util::CJsonObject objLog;
        if (!objLog.Parse(strLogInfo))
        {
            LOG4_WARN("%s() failed to parse strLogInfo:%s",__FUNCTION__,strLogInfo.c_str());
            return;
        }
        LOG4_INFO("%s() TestWriteLogs AppendLog m_uiTestWriteTime(%u) strLogInfo.size(%u) sizeof(LogDataHeader):%u objLog:%s",
                                __FUNCTION__,m_uiTestWriteTime,strLogInfo.size(),sizeof(LogDataHeader),objLog.ToString().c_str());
        behaviour::behaviour oInAsk;
        oInAsk.set_type("trace");
        oInAsk.set_log_info(objLog.ToString());//不含回车
        net::uint32 uiCounter(0);
        for(;uiCounter < m_uiTestWriteTime;++uiCounter)
        {
            int nErrCode(0);
            if (!AppendLog(oInAsk,nErrCode))
            {
                LOG4_WARN("%s() failed to AppendLog nErrCode(%d),uiCounter(%u)",__FUNCTION__,nErrCode,uiCounter);
                break;
            }
        }
        LOG4_INFO("%s() TestWriteLogs AppendLog uiSyncLog(%u) uiCounter(%u) strLogInfo.size(%u) sizeof(LogDataHeader):%u log_info:%u",
                        __FUNCTION__,m_uiSyncLog,uiCounter,strLogInfo.size(),sizeof(LogDataHeader),oInAsk.log_info().size());
        m_CustomClock.EndClock();
        /*
                             每个消息同步写
         TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
         CustomClock TestWriteLogs use time(2561.010010) ms
                         文件6.68mb
         qps:3904.7
                           吞吐量：2.6 Mb/s

                            每个消息同步写 + 加合法性检查
        TestWriteLogs() TestWriteLogs AppendLog uiSyncLog(1) uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
        CustomClock TestWriteLogs use time(2635.263916) ms
                             文件6.68mb
         qps:3794.6
                           吞吐量：2.53 Mb/s

                     每个消息异步写
       TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
       CustomClock TestWriteLogs use time(84.639000) ms
                    文件6.68mb
        qps:118147.4
                           吞吐量：78.9 Mb/s

                            每个消息异步写 + 加合法性检查
         TestWriteLogs() TestWriteLogs AppendLog uiCounter(10000) strLogInfo.size(693) sizeof(LogDataHeader):8 log_info:693
         CustomClock TestWriteLogs use time(184.009995) ms
                       文件6.68mb
        qps:54347.8
                           吞吐量：36.3 Mb/s
         * */
    }
}

bool LogQueueSession::CheckOpenNewLog()
{
    SetCurrentTime();
    //每隔一段时间创建一个新的日志文件
    if(m_createLogLastTime + m_createLogInterval <= m_currentTime)
    {
        LOG4CPLUS_TRACE_FMT(GetLogger(),"%s() TryOpenNewLog,createLogInterval(%lf) m_createLogLastTime(%u) m_currentTime(%u)",
                __FUNCTION__,m_createLogInterval,m_createLogLastTime,m_currentTime);
        if (m_localFileMgr.m_logFile.IsOpened() && m_localFileMgr.m_logFile.GetFileSize() > 0)
        {//旧文件存在且大小大于0 时 才创建新文件
            if (m_localFileMgr.TryOpenNewLog())
            {
                if (m_localFileMgr.GetCreatLogLastTime() != m_createLogLastTime)
                {
                    m_createLogLastTime = m_localFileMgr.GetCreatLogLastTime();//真正成功创建文件的更新创建日志文件时间
                }
                return true;
            }
            return false;
        }
    }
    return true;
}

bool LogQueueSession::AppendLog(behaviour::behaviour &message,int& nErrCode)
{
    if (message.log_info().size() == 0)
    {
        LOG4CPLUS_WARN_FMT(GetLogger(), "%s() message.log_info().size() == 0",__FUNCTION__);
        nErrCode = ERR_INVALID_PARAMS;
        return false;
    }
    if (m_uiVerifyLog > 0)
    {//校验字段合法性
        std::map<std::string,LogTable>::const_iterator cit = m_mapLogTypes.find(message.type());
        if (m_mapLogTypes.end() == cit)
        {
            LOG4CPLUS_WARN_FMT(GetLogger(), "%s() log_type(%s) invalid",__FUNCTION__,message.type().c_str());
            nErrCode = ERR_INVALID_PARAMS;
            return false;
        }
        const LogTable& oLogTable = cit->second;
        message.set_log_cmd(oLogTable.nLogCmd);
        m_clientMsg.Clear();
        if (!m_clientMsg.Parse(message.log_info()))
        {
            LOG4CPLUS_WARN_FMT(GetLogger(), "%s() invalid data.failed to parse log_info:%s",
                            __FUNCTION__,message.log_info().c_str());
            nErrCode = ERR_INVALID_PARAMS;
            return false;
        }
        if (m_clientMsg.GetArraySize() > 0)
        {
            int nArraySize = m_clientMsg.GetArraySize();
            for(int i = 0;i < nArraySize;++i)
            {
                std::vector<std::string>::const_iterator citField = oLogTable.fieldsVec.begin();
                std::vector<std::string>::const_iterator citFieldEnd = oLogTable.fieldsVec.end();
                for(;citField != citFieldEnd;++citField)
                {
                    if(m_clientMsg[i](*citField).size() == 0)
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
                if(m_clientMsg(*citField).size() == 0)
                {
                    LOG4CPLUS_WARN_FMT(GetLogger(), "%s() invalid data.failed to get field:%s,log_info:%s",
                                    __FUNCTION__,citField->c_str(),message.log_info().c_str());
                    nErrCode = ERR_INVALID_PARAMS;
                    return false;
                }
            }
        }
        LOG4CPLUS_DEBUG_FMT(GetLogger(), "%s() succ to validate log_info:%s",
                                    __FUNCTION__,message.log_info().c_str());
    }
    else
    {
        LOG4CPLUS_DEBUG_FMT(GetLogger(), "%s() need to verify log in CollectServer log_cmd:%u",
                        __FUNCTION__,message.log_cmd());
        if (message.log_cmd() == 0)
        {
            LOG4CPLUS_WARN_FMT(GetLogger(), "%s() message.log_cmd() == 0,message:%s",
                            __FUNCTION__,message.DebugString().c_str());
            nErrCode = ERR_INVALID_PARAMS;
            return false;
        }
    }
    SetCurrentTime();
    bool boForceNewLog(false);
    {
        //每隔一段时间创建一个新的日志文件
        if(m_createLogLastTime + m_createLogInterval <= m_currentTime)
        {
            if (m_localFileMgr.m_logFile.GetFileSize() != 0)
            {
                //旧文件大小为0 时 不需要创建新文件
                boForceNewLog = true;
                LOG4CPLUS_TRACE_FMT(GetLogger(),"need New Log,createLogInterval(%lf) m_createLogLastTime(%u) m_currentTime(%u)",
                        m_createLogInterval,m_createLogLastTime,m_currentTime);
            }
        }
    }
    bool boAppend(false);
    if (0.0 == m_writeLogInterval)
    {
        LOG4CPLUS_DEBUG_FMT(GetLogger(),"%s() write immediately",__FUNCTION__);
        boAppend = m_localFileMgr.AddLog(message,true,boForceNewLog);//添加到日志队列
        m_writeLogLastTime = m_currentTime;
    }
    else if (m_writeLogLastTime + m_writeLogInterval > m_currentTime)//延时添加
    {
        LOG4CPLUS_DEBUG_FMT(GetLogger(),"%s() append log queue time(%u,%lf,%u)",__FUNCTION__,m_writeLogLastTime,m_writeLogInterval,m_currentTime);
        boAppend = m_localFileMgr.AddLog(message,false,boForceNewLog);//添加到日志队列
    }
    else
    {
        boAppend = m_localFileMgr.AddLog(message,true,boForceNewLog);//添加到日志队列
        m_writeLogLastTime = m_currentTime;
    }

    if (m_localFileMgr.GetCreatLogLastTime() > 0)
    {
        m_createLogLastTime = m_localFileMgr.GetCreatLogLastTime();//真正成功创建文件的更新创建日志文件时间
    }
    if (boAppend)
    {
        nErrCode = ERR_RESPONSE_OK;
    }
    else
    {
        nErrCode = ERR_SERVER_LOGIC_ERROR;
    }
    return boAppend;
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
        pSess->SetConfigPath(strConfigPath);
        pSess->SetWorkerIdentify(strWorkerIdentify);
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
