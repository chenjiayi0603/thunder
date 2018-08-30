/*
 * PgAgentSession.h
 *
 *  Created on: 2018年1月8日
 *      Author: chen
 */
#ifndef CODE_SRC_DBAGENTSSESSION_H_
#define CODE_SRC_DBAGENTSSESSION_H_
#include <string>
#include <map>

#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "../../../l3oss/include/postgresql/postgresql/server/port/win32_msvc/unistd.h"
#include "util/json/CJsonObject.hpp"
#include "session/Session.hpp"
#include "OssDefine.hpp"
#include "OssError.hpp"
#include "step/Step.hpp"
#include "cmd/Cmd.hpp"

#define DBAGENT_SESSIN_ID (20000)

namespace oss
{
class CmdPgOper;
class PgAgentSession: public oss::Session
{
public:
    PgAgentSession(double session_timeout = 1.0)
                    : oss::Session(DBAGENT_SESSIN_ID, session_timeout,"oss::PgAgentSession"),
                      m_uiCurrentTime(0)
    {
    }
    virtual ~PgAgentSession()
    {
    }
    oss::E_CMD_STATUS Timeout();

    //配置
    void SetConfigPath(const std::string &configpath){m_strConfigPath = configpath;}
    void SetWorkerIdentify(const std::string &workerIdentify){m_strWorkerIdentify = workerIdentify;}
    const std::string& GetWorkerIdentify()const {return m_strWorkerIdentify;}

    //配置
    std::string m_strConfigPath;
    std::string m_strWorkerIdentify;
private:
    uint32 m_uiCurrentTime;
};

PgAgentSession* GetPgAgentSession(oss::OssLabor* pLabor);

}
;

#endif /* CODE_WEBSERVER_SRC_WEBSESSION_H_ */
