/*
 * DbAgentSession.cpp
 *
 *  Created on: 2018年1月8日
 *      Author: chenjiayi
 */
#include "DbAgentSession.h"
#include "CmdDbOper.hpp"

namespace oss
{

DbAgentSession* GetDbAgentSession(oss::OssLabor* pLabor)
{
    DbAgentSession* pSess = (DbAgentSession*) pLabor->GetSession(DBAGENT_SESSIN_ID,"oss::DbAgentSession");
    if (pSess)
    {
        return (pSess);
    }
    pSess = new DbAgentSession();
    if (pSess == NULL)
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "error %d: new DbAgentSession() error!",
                        oss::ERR_NEW);
        return (NULL);
    }
    if (pLabor->RegisterCallback(pSess))
    {
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(), "register DbAgentSession ok!");
        return (pSess);
    }
    else
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "register DbAgentSession error!");
        delete pSess;
        pSess = NULL;
    }
    return (NULL);
}

oss::E_CMD_STATUS DbAgentSession::Timeout()
{
    LOG4CPLUS_DEBUG_FMT(GetLogger(), "%s() check db keepalive.size(%u)",
                    __FUNCTION__,pCmdDbOper->m_mapDbiPool.size());
    {
        std::map<std::string, tagConnection*>::iterator iter = pCmdDbOper->m_mapDbiPool.begin();
        std::map<std::string, tagConnection*>::iterator iterEnd = pCmdDbOper->m_mapDbiPool.end();
        for(;iter != iterEnd;++iter)
        {
            if (iter->second->pDbi->MysqlPing() != 0)
            {
                LOG4CPLUS_WARN_FMT(GetLogger(), "%s() mysql(%s,%u) lost connection",__FUNCTION__,
                                iter->second->pDbi->GetDbConf().m_stDbConnInfo.m_szDbHost,
                                iter->second->pDbi->GetDbConf().m_stDbConnInfo.m_uiDbPort);
                iter->second->ullBeatTime = 0;
            }
            else
            {
                iter->second->ullBeatTime = ::time(NULL);
            }
        }
    }
    return oss::STATUS_CMD_RUNNING;
}

}
;
//namespace starshiplib
