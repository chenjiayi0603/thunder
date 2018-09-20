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
#include "FileMgr.h"

#define LOGQUEUE_SESSIN_ID (20000)

namespace analysis
{

class LogQueueSession: public net::Session
{
public:
    LogQueueSession(double session_timeout = 1.0)
                    : net::Session(LOGQUEUE_SESSIN_ID, session_timeout,"analysis::LogQueueSession"),
                      boInit(false),boTestWriteLogs(false),m_uiVerifyLog(0),m_uiCurrentTime(0),m_uiTestWriteTime(0)
    {
    }
    virtual ~LogQueueSession(){}
    bool Init(const util::CJsonObject& conf,const util::CJsonObject& logTableConf);
    net::E_CMD_STATUS Timeout();

    void TestWriteLogs();
    uint32 SetCurrentTime(){m_uiCurrentTime = ::time(NULL);return m_uiCurrentTime;}
    bool AppendLog(behaviour::behaviour &message,int& nErrCode);
    bool VerifyLog(behaviour::behaviour &message,int& nErrCode);
    bool CheckOpenNewLog();

    std::string m_strConfigPath;
	std::string m_strWorkerIdentify;
private:
    bool boInit;
    bool boTestWriteLogs;
    uint32 m_uiVerifyLog;
    uint32 m_uiCurrentTime;
    uint32 m_uiTestWriteTime;

    //日志
    FileMgr m_FileMgr;

    std::map<std::string,LogTable> m_mapLogTable;//logtype -> LogTable
    util::CJsonObject m_jsonLogMsg;
};

LogQueueSession* GetLogQueueSession(net::Labor* pLabor,const std::string &strConfigPath,const std::string& strWorkerIdentify);

}
;

#endif /* CODE_WEBSERVER_SRC_WEBSESSION_H_ */
