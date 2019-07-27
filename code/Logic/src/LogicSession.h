/*
 * RobotSession.h
 *
 *  Created on: 2015年10月21日
 *      Author: chen
 */
#ifndef CODE_WEBSERVER_SRC_WEBSESSION_H_
#define CODE_WEBSERVER_SRC_WEBSESSION_H_
#include <string>
#include <map>
#include <sys/time.h>
#include "RobotErrorMapping.h"
#include "session/Session.hpp"
#include "NetError.hpp"
#include "RobotError.h"
#include "step/Step.hpp"
#include "cmd/Cmd.hpp"
#include "Define.h"
#include <unordered_map>
#include "util/CommonUtils.hpp"

#define ROBOT_SESSIN_ID (1000)

namespace robot
{

struct Token
{
	Token()
	{
		strToken = std::to_string(util::GetUniqueId(net::GetNodeId(),net::GetWorkerIndex()));
		m_uiTimeCreate = ::time(NULL);
		m_uiTimeOut = m_uiTimeCreate + 40;
	}
	std::string strToken;
	uint32 m_uiTimeOut;
	uint32 m_uiTimeCreate;
};

class LogicSession: public net::Session
{
public:
	LogicSession(uint64 ulSessionId, ev_tstamp dSessionTimeout, const std::string& strSessionClass)
	: net::Session(ulSessionId, dSessionTimeout,strSessionClass),
	  boInit(false),m_currenttime(0){}
    virtual ~LogicSession(){}
    bool Init(const util::CJsonObject& conf);
    net::E_CMD_STATUS Timeout();
    void setCurrentTime(){m_currenttime = ::time(NULL);}
    uint32 getCurrentTime(){return m_currenttime;}
    Token GenToken(const std::string& address)
    {
    	auto token = Token();
    	m_tokenM[address] = token;
    	return token;
    }
private:
    bool boInit;
    uint32 m_currenttime; //当前时间
    std::map<std::string,Token> m_tokenM;
};

LogicSession* GetLogicSession();

extern LogicSession* g_pLogicSession;

}

#endif /* CODE_WEBSERVER_SRC_WEBSESSION_H_ */
