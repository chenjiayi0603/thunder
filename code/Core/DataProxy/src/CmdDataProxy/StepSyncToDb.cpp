/*******************************************************************************
 * Project:  DataProxy
 * @file     StepSyncToDb.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年7月21日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepSyncToDb.hpp"

namespace net
{
static uint64 g_uiSyncCounter = 0;

StepSyncToDb::StepSyncToDb(const std::string& strSessionId, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
    : m_strSessionId(strSessionId), m_oMsgHead(oInMsgHead), m_oMsgBody(oInMsgBody),m_timeOut(0)
{
}

StepSyncToDb::~StepSyncToDb()
{
}

E_CMD_STATUS StepSyncToDb::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
	MsgHead oMsgHead = m_oMsgHead;
	oMsgHead.set_seq(GetSequence());//替换为新消息头发送
    LOG4_TRACE("try to sync strTableName(%s),oMsgHead(%d:%s),oMsgBody(%d),cmd(%u)",
                    m_strSessionId.c_str(),m_oMsgHead.ByteSize(),m_oMsgHead.DebugString().c_str(),
					m_oMsgBody.ByteSize(),m_oMsgHead.cmd());
    if (!SendToNext("DBAGENT_W", oMsgHead, m_oMsgBody))
    {
        LOG4_ERROR("SendToNext(\"DBAGENT_W\") error!");
        SessionSyncDbData* pSessionSync = (SessionSyncDbData*)GetSession(m_strSessionId, "net::SessionSyncDbData");
        if (NULL != pSessionSync)
        {
            pSessionSync->GoBack(m_oMsgHead, m_oMsgBody);//回退数据到缓存,延时后继续尝试写dbagent
        }
        return(STATUS_CMD_FAULT);
    }
    return(STATUS_CMD_RUNNING);
}

E_CMD_STATUS StepSyncToDb::Callback(const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, void* data)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    DataMem::MemRsp oRsp;
    if (!oRsp.ParseFromString(oInMsgBody.body()))
    {
        LOG4_ERROR("DataMem::MemRsp oRsp.ParseFromString() failed!");
        return(STATUS_CMD_FAULT);
    }

    SessionSyncDbData* pSessionSync = (SessionSyncDbData*)GetSession(m_strSessionId, "net::SessionSyncDbData");
    if (ERR_OK != oRsp.err_no())
    {
        LOG4_ERROR("%d: %s", oRsp.err_no(), oRsp.err_msg().c_str());
        //if (DataMem::MemOperate::DbOperate::SELECT != oMemOperate.db_operate().query_type()
        if ((oRsp.err_no() >= 2001 && oRsp.err_no() <= 2018)
                        || (oRsp.err_no() >= 2038 && oRsp.err_no() <= 2046))
        {   // 由于连接方面原因数据写失败，先写入文件，等服务从故障中恢复后再自动重试
            if (NULL != pSessionSync)
            {
                pSessionSync->GoBack(m_oMsgHead, m_oMsgBody);//回退数据到缓存,延时后继续尝试写dbagent（这里目前没有写文件）
            }
            return(STATUS_CMD_FAULT);
        }
    }
    ++g_uiSyncCounter;
    LOG4_TRACE("succ to sync strTableName(%s),oMsgHead ByteSize(%d),oMsgBody ByteSize(%d),cmd(%u),syncCounter(%llu)",
                            m_strSessionId.c_str(),m_oMsgHead.ByteSize(),m_oMsgBody.ByteSize(),m_oMsgHead.cmd(),g_uiSyncCounter);
    if (NULL != pSessionSync)
    {
        LOG4_TRACE("count for SyncData strTableName(%s):WritedCounter(%u),ReadCounter(%u),syncCounter(%llu)",
                        m_strSessionId.c_str(),pSessionSync->GetWritedCounter(),pSessionSync->GetReadCounter(),g_uiSyncCounter);
        if (pSessionSync->GetSyncData(m_oMsgHead, m_oMsgBody))//获取同步文件中的数据到缓存后尝试发送dbagent
        {
            return(Emit(ERR_OK));
        }
    }
    LOG4_INFO("done sync strTableName(%s),oMsgHead ByteSize(%d),oMsgBody ByteSize(%d),cmd(%u),syncCounter(%llu)",
							m_strSessionId.c_str(),m_oMsgHead.ByteSize(),m_oMsgBody.ByteSize(),m_oMsgHead.cmd(),g_uiSyncCounter);
    return(STATUS_CMD_COMPLETED);
}

E_CMD_STATUS StepSyncToDb::Timeout()
{
    ++m_timeOut;
    if(m_timeOut > 3)
    {
        return(STATUS_CMD_FAULT);
    }
    return(STATUS_CMD_RUNNING);
}

} /* namespace net */
