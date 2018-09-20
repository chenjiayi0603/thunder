/*
 * HelloSession.cpp
 *
 *  Created on: 2016骞�11鏈�23鏃�
 *      Author: chenjiayi
 */
#include "HelloSession.h"

namespace core
{

bool HelloSession::Init(const util::CJsonObject& conf)
{
    if(boInit)
    {
        return true;
    }
    conf.Get("module_locate_data_request", m_objModuleLocateDataRequest);
    conf.Get("access_control_allow_origin",m_AccessControlAllowOrigin);
    conf.Get("access_control_allow_headers",m_AccessControlAllowHeaders);
    conf.Get("access_control_allow_methods",m_AccessControlAllowMethods);
    if (!conf.Get("valid_time_delay", m_ValidTimeDelay))
    {
        m_ValidTimeDelay = 60;
    }
    LOG4_INFO("%s valid time delay:%d",__FUNCTION__,m_ValidTimeDelay);
    LOG4CPLUS_DEBUG_FMT(GetLogger(),"%s() objModuleLocateDataRequest(%s)",
                        __FUNCTION__,m_objModuleLocateDataRequest.ToString().c_str());
    SetCurrentTime();
    boInit = true;
    return true;
}

HelloSession* GetHelloSession(net::Labor* pLabor,const std::string &configPath)
{
    HelloSession* pSess = (HelloSession*) pLabor->GetSession(HELLO_SESSIN_ID);
    if (pSess)
    {
        return (pSess);
    }
    pSess = new HelloSession();
    if (pSess == NULL)
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "error %d: new HelloSession() error!",
                        net::ERR_NEW);
        return (NULL);
    }
    util::CJsonObject   oCurrentConf;
    {
        std::string strConfFile = configPath + std::string("/HelloCmd.json");
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(), "CONF FILE = %s.", strConfFile.c_str());

        std::ifstream fin(strConfFile.c_str());
        if (fin.good())
        {
            std::stringstream ssContent;
            ssContent << fin.rdbuf();
            if (!oCurrentConf.Parse(ssContent.str()))
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
        pSess->SetConfigPath(configPath);
    }
    if (pLabor->RegisterCallback(pSess))
    {
        if (!pSess->Init(oCurrentConf))
        {
            pLabor->DeleteCallback(pSess);
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "HelloSession init error!");
            return (NULL);
        }
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(), "register HelloSession ok!");
        return (pSess);
    }
    else
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "register HelloSession error!");
        delete pSess;
        pSess = NULL;
    }
    return (NULL);
}


}
;
//name space robot
