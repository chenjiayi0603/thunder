#include "StepAuthRedis.hpp"

namespace net
{
StepAuthRedis::StepAuthRedis(const DataMem::MemOperate::RedisOperate& oRedisOperate,tagRedisAttr*& ptagRedisAttr):
        net::RedisStep(),m_oRedisOperate(oRedisOperate),m_ptagRedisAttr(ptagRedisAttr)
{
	Build();
}

void StepAuthRedis::Build()
{
	UnsetRegistered();
	util::RedisCmd* pRedisCmd = RedisCmd();
	if (pRedisCmd && pRedisCmd->GetCmd().size() == 0)
	{
		pRedisCmd->SetCmd(m_oRedisOperate.redis_cmd_read().size() > 0 ?m_oRedisOperate.redis_cmd_read():m_oRedisOperate.redis_cmd_write());
		if (m_oRedisOperate.key_name().size() > 0)
		{
			pRedisCmd->Append(m_oRedisOperate.key_name());
		}
		for (int i = 0; i < m_oRedisOperate.fields_size(); ++i)
		{
			if (m_oRedisOperate.fields(i).col_name().size() > 0)
			{
				pRedisCmd->Append(m_oRedisOperate.fields(i).col_name());
			}
			if (m_oRedisOperate.fields(i).col_value().size() > 0)
			{
				pRedisCmd->Append(m_oRedisOperate.fields(i).col_value());
			}
		}
		LOG4_TRACE("%s() redis_cmd_read(%s) redis_cmd_write(%s) pRedisCmd(%s) oRedisOperate(%s)",__FUNCTION__,
				m_oRedisOperate.redis_cmd_read().c_str(),m_oRedisOperate.redis_cmd_write().c_str(),pRedisCmd->ToString().c_str(),
				m_oRedisOperate.DebugString().c_str());
	}
}

net::E_CMD_STATUS StepAuthRedis::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
//    LOG4_TRACE("%s()", __FUNCTION__);
	++m_iEmitNum;
	return(net::STATUS_CMD_RUNNING);//在worker处理
}

net::E_CMD_STATUS StepAuthRedis::Callback(const redisAsyncContext *c, int status, redisReply* pReply)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    char szErrMsg[256] = {0};
    if (REDIS_OK != status)
    {
        snprintf(szErrMsg, sizeof(szErrMsg), "redis cmd status %d!",status);
        LOG4_WARN("%s() failed.szErrMsg:%s", __FUNCTION__,szErrMsg);
        m_ptagRedisAttr->bIsAuthFail = true;
        return(net::STATUS_CMD_FAULT);
    }
    if (nullptr == pReply)
    {
        snprintf(szErrMsg, sizeof(szErrMsg), "redis error %d: %s!",  c->err, c->errstr);
        LOG4_WARN("%s() failed.szErrMsg:%s", __FUNCTION__,szErrMsg);
        m_ptagRedisAttr->bIsAuthFail = true;
        return(net::STATUS_CMD_FAULT);
    }
    LOG4_TRACE("redis reply->type = %d", pReply->type);//redis reply->type = 1
    if (REDIS_REPLY_ERROR == pReply->type)
    {
        snprintf(szErrMsg, sizeof(szErrMsg), "redis error %d: %s!",  pReply->type, pReply->str);
        LOG4_WARN("%s() failed.szErrMsg:%s", __FUNCTION__,szErrMsg);
        m_ptagRedisAttr->bIsAuthFail = true;
        return(net::STATUS_CMD_FAULT);
    }
//    if (REDIS_REPLY_NIL == pReply->type)
//    {
//        LOG4_TRACE("%s() ok.pReply:null.", __FUNCTION__);
//        m_uiPingNode = 1;
//        return(net::STATUS_CMD_COMPLETED);
//    }
//    if (REDIS_REPLY_ARRAY == pReply->type)
//    {
//        for(uint32 i = 0;i < pReply->elements;++i)
//        {
//            redisReply* pElememtReply = pReply->element[i];
//            LOG4_TRACE("%s() ok.pElememtReply:%s", __FUNCTION__,pElememtReply->str);
//        }
//        m_uiPingNode = 1;
//        return(net::STATUS_CMD_COMPLETED);
//    }
    if (m_ptagRedisAttr)
    {
    	m_ptagRedisAttr->bIsReady = true;
    }
    LOG4_TRACE("%s() ok.pReply:%s", __FUNCTION__,pReply->str);
    return(net::STATUS_CMD_COMPLETED);
}

}
