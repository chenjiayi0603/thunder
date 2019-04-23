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

#include "ProtoError.h"
#include "log.pb.h"
#include "util/json/CJsonObject.hpp"
#include "session/Session.hpp"
#include "NetDefine.hpp"
#include "NetError.hpp"
#include "step/Step.hpp"
#include "cmd/Cmd.hpp"
#include "Comm.hpp"
#include "FileMgr.h"

#define LOGQUEUE_SESSIN_ID (20000)

namespace core
{

class LogQueueSession: public net::Session
{
public:
    LogQueueSession(double session_timeout = 1.0)
                    : net::Session(LOGQUEUE_SESSIN_ID, session_timeout,"analysis::LogQueueSession"),
                      boInit(false),m_uiCurrentTime(0),m_uiEnableTest(0),m_uiTestWriteTime(0),m_uiVerifyLog(0)
    {
    }
    virtual ~LogQueueSession(){}
    bool Init(const util::CJsonObject& conf,const util::CJsonObject& logTableConf);
    net::E_CMD_STATUS Timeout();

    void TestWriteLogs();
    uint32 SetCurrentTime(){m_uiCurrentTime = ::time(NULL);return m_uiCurrentTime;}
    int AppendLog(logqueue::log &message);
    int VerifyLog(logqueue::log &message);
    bool CheckOpenNewLog();
private:
    bool boInit;
    uint32 m_uiCurrentTime;
    uint32 m_uiEnableTest;
    uint32 m_uiTestWriteTime;
    uint32 m_uiVerifyLog;

    //日志
    FileMgr m_FileMgr;
};

LogQueueSession* GetLogQueueSession();

}
;

#endif /* CODE_WEBSERVER_SRC_WEBSESSION_H_ */
