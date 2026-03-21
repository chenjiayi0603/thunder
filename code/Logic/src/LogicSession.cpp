/*
 * LogicSession.cpp
 *
 *  Created on: 2017年1月19日
 *      Author: chenjiayi
 */
#include "LogicSession.h"
#include "util/json/CJsonObject.hpp"

namespace robot
{

namespace
{
LogicSession* g_pCachedLogicSession = nullptr;
}

LogicSession::~LogicSession()
{
    if (g_pCachedLogicSession == this)
    {
        g_pCachedLogicSession = nullptr;
    }
}

bool LogicSession::Init(const util::CJsonObject& conf)
{
    if(boInit)return true;
    setCurrentTime();
    boInit = true;
    return true;
}

net::E_CMD_STATUS LogicSession::Timeout()
{
	setCurrentTime();
	auto iter = m_tokenM.begin();
	while(iter != m_tokenM.end())
	{
		if (iter->second.m_uiTimeOut <= m_currenttime)
		{
			LOG4_INFO("strToken(%s) has been time out.(%u,%u)!",
					iter->second.strToken.c_str(),iter->second.m_uiTimeOut,m_currenttime);
			m_tokenM.erase(iter++);
		}
		else
		{
			iter++;
		}
	}
	return net::STATUS_CMD_RUNNING;
}

LogicSession* GetLogicSession()
{
	if (g_pCachedLogicSession) return g_pCachedLogicSession;
	LOG4_INFO("GetWorkPath(%s) GetConfigPath(%s)!",GetLabor()->GetWorkPath().c_str(),net::GetConfigPath().c_str());
	std::string strConfigPath = net::GetConfigPath() + std::string("LogicCmd.json");
    util::CJsonObject oCurrentConf;       ///< 当前加载的配置
    if (!net::GetConfig(oCurrentConf,strConfigPath))
    {
    	LOG4_ERROR("Open conf (%s) error!",strConfigPath.c_str());
    	return NULL;
    }
	LOG4_INFO("Open conf (%s)!",oCurrentConf.ToString().c_str());
    return (g_pCachedLogicSession = net::MakeSession<LogicSession>(ROBOT_SESSIN_ID,std::string("robot::LogicSession"),1.0,oCurrentConf));
}


}
;
//name space robot
