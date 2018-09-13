/*
 * PgAgentSession.cpp
 *
 *  Created on: 2018年1月8日
 *      Author: chenjiayi
 */
#include "PgAgentSession.h"
#include "CmdPgOper.hpp"

namespace net
{

PgAgentSession* GetPgAgentSession(net::Labor* pLabor)
{
    PgAgentSession* pSess = (PgAgentSession*) pLabor->GetSession(DBAGENT_SESSIN_ID,"net::PgAgentSession");
    if (pSess)
    {
        return (pSess);
    }
    pSess = new PgAgentSession();
    if (pSess == NULL)
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "error %d: new PgAgentSession() error!",
                        net::ERR_NEW);
        return (NULL);
    }
    if (pLabor->RegisterCallback(pSess))
    {
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(), "register PgAgentSession ok!");
        return (pSess);
    }
    else
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(), "register PgAgentSession error!");
        delete pSess;
        pSess = NULL;
    }
    return (NULL);
}

net::E_CMD_STATUS PgAgentSession::Timeout()
{
//    LOG4CPLUS_DEBUG_FMT(GetLogger(), "%s() check db keepalive.size(%u)",
//                    __FUNCTION__,pCmdDbOper->m_mapDbiPool.size());
//    {
//        std::map<std::string, tagConnection*>::iterator iter = pCmdDbOper->m_mapDbiPool.begin();
//        std::map<std::string, tagConnection*>::iterator iterEnd = pCmdDbOper->m_mapDbiPool.end();
//        for(;iter != iterEnd;++iter)
//        {
//            if (iter->second->pPgConn->MysqlPing() != 0)
//            {
//                LOG4CPLUS_WARN_FMT(GetLogger(), "%s() mysql(%s,%u) lost connection",__FUNCTION__,
//                                iter->second->pPgConn->GetDbConf().m_stDbConnInfo.m_szDbHost,
//                                iter->second->pPgConn->GetDbConf().m_stDbConnInfo.m_uiDbPort);
//                iter->second->ullBeatTime = 0;
//            }
//            else
//            {
//                iter->second->ullBeatTime = ::time(NULL);
//            }
//        }
//    }
    return net::STATUS_CMD_RUNNING;
}

}
;
//namespace core
