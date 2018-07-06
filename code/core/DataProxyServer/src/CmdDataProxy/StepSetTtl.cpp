/*******************************************************************************
 * Project:  DataProxyServer
 * @file     StepSetTtl.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月26日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepSetTtl.hpp"

namespace oss
{

StepSetTtl::StepSetTtl(const std::string& strMasterNodeIdentify, const std::string& strKey, int32 iExpireSeconds)
    : m_strMasterNodeIdentify(strMasterNodeIdentify), m_strKey(strKey), m_iExpireSeconds(iExpireSeconds)
{
}

StepSetTtl::~StepSetTtl()
{
}

E_CMD_STATUS StepSetTtl::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    loss::RedisCmd* pRedisCmd = RedisCmd();
    if (pRedisCmd)
    {
        pRedisCmd->SetCmd("EXPIRE");
        pRedisCmd->Append(m_strKey);
        char szExpireSeconds[32] = {0};
        snprintf(szExpireSeconds, 32, "%d", m_iExpireSeconds);
        pRedisCmd->Append(szExpireSeconds);
        if (RegisterCallback(m_strMasterNodeIdentify, this))
        {
            return(STATUS_CMD_RUNNING);
        }
        LOG4_ERROR("RegisterCallback(%s, StepSetTtl) error!", m_strMasterNodeIdentify.c_str());
    }
    return(STATUS_CMD_FAULT);
}

E_CMD_STATUS StepSetTtl::Callback(const redisAsyncContext *c, int status, redisReply* pReply)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (REDIS_OK != status)
    {
        LOG4_ERROR("redis cmd status %d!", status);
        return(STATUS_CMD_FAULT);
    }
    if (NULL == pReply)
    {
        LOG4_ERROR("error %d: %s!", c->err, c->errstr);
        return(STATUS_CMD_FAULT);
    }
    LOG4_TRACE("redis reply->type = %d", pReply->type);
    if (REDIS_REPLY_ERROR == pReply->type)
    {
        LOG4_ERROR("error %d: %s!", pReply->type, pReply->str);
        return(STATUS_CMD_FAULT);
    }
    LOG4_TRACE("OK");
    return(STATUS_CMD_COMPLETED);
}

} /* namespace oss */
