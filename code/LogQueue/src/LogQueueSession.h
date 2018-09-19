/*
 * FileSession.h
 *
 *  Created on: 2015年10月21日
 *      Author: chen
 */
#ifndef CODE_LOGQUEUE_SRC_LOG_QUEUESESSION_H_
#define CODE_LOGQUEUE_SRC_LOG_QUEUESESSION_H_
#include <string>
#include <map>
#include <vector>

#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "AnalysisError.h"
#include "AnalysisErrorMapping.h"
#include "behaviour.pb.h"
#include "util/json/CJsonObject.hpp"
#include "session/Session.hpp"
#include "NetDefine.hpp"
#include "NetError.hpp"
#include "step/Step.hpp"
#include "cmd/Cmd.hpp"
#include "Comm.hpp"
#include "LocalFileMgr.h"

#define LOGQUEUE_SESSIN_ID (20000)

namespace analysis
{

class LogQueueSession: public net::Session
{
public:
    LogQueueSession(double session_timeout = 1.0)
                    : net::Session(LOGQUEUE_SESSIN_ID, session_timeout,"analysis::LogQueueSession"),
                      boInit(false),boTestWriteLogs(false),m_currentTime(0),m_writeLogInterval(0),m_createLogInterval(0),
                      m_uiVerifyLog(0),m_uiSyncLog(1),m_logQueueNum(0),m_uiLogFormat(0),
                      m_uiTestWriteTime(0),m_writeLogLastTime(0),m_createLogLastTime(0)
    {
    }
    virtual ~LogQueueSession()
    {
    }
    bool Init(const util::CJsonObject& conf,const util::CJsonObject& logTableConf);
    net::E_CMD_STATUS Timeout()
    {
        if (m_localFileMgr.GetAllWriteSize() > 0)
        {
            CheckOpenNewLog();
            m_localFileMgr.RoutineWrite(false);
        }
        m_localFileMgr.CheckSync();
        return net::STATUS_CMD_RUNNING;
    }
    void SetConfigPath(const std::string &configpath)
    {
        m_strConfigPath = configpath;
    }
    void SetWorkerIdentify(const std::string &workerIdentify)
    {
        m_strWorkerIdentify = workerIdentify;
    }
    void SetLogQueueNum(uint32 logQueueNum)
    {
        m_logQueueNum = logQueueNum;
    }
    const std::string& GetWorkerIdentify()const {return m_strWorkerIdentify;}
    void SetCurrentTime()
    {
        m_currentTime = ::time(NULL);
    }
    uint64 getCurrentTime()
    {
        return m_currentTime;
    }
    void TestWriteLogs();
    bool AppendLog(behaviour::behaviour &message,int& nErrCode);
    bool CheckOpenNewLog();
    /*
     "log_list":[
        {
            "log_cmd":1,
            "log_type":"appStartTrace",
            "fields": [
            ]
        },
        {
            "log_cmd":2,
            "log_type":"appPageTrace",
            "fields": [
            ]
        }
    ]
     * */
    struct LogTable
    {
        uint32 nLogCmd;
        std::string strLogType;
        std::vector<std::string> fieldsVec;
        LogTable():nLogCmd(0){}
        LogTable(const LogTable& log)
        {
            nLogCmd = log.nLogCmd;
            strLogType = log.strLogType;
            fieldsVec = log.fieldsVec;
        }
        const LogTable& operator=(const LogTable& log)
        {
            nLogCmd = log.nLogCmd;
            strLogType = log.strLogType;
            fieldsVec = log.fieldsVec;
            return *this;
        }
    };
    std::vector<util::CJsonObject> m_vecComsumeTables;
private:
    bool boInit;
    bool boTestWriteLogs;
    net::uint32 m_currentTime;
    std::string m_strConfigPath;
    std::string m_strWorkerIdentify;
    //config
    std::string m_datalogPath; //日志文件目录 路径
    double m_writeLogInterval; //定时写日志间隔
    double m_createLogInterval; //创建日志文件间隔

    net::uint32 m_uiVerifyLog;
    net::uint32 m_uiSyncLog;//同步写日志
    net::uint32 m_logQueueNum;
    uint32 m_uiLogFormat;

    uint32 m_uiTestWriteTime;
    uint32 m_writeLogLastTime;
    uint32 m_createLogLastTime;

    CustomClock m_CustomClock;

    //日志
    LocalFileMgr m_localFileMgr;

    std::map<std::string,LogTable> m_mapLogTypes;//logtype -> LogTable
    util::CJsonObject m_clientMsg;
};

LogQueueSession* GetLogQueueSession(net::Labor* pLabor,const std::string &strConfigPath,const std::string& strWorkerIdentify);

}
;

#endif /* CODE_WEBSERVER_SRC_WEBSESSION_H_ */
