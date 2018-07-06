/*******************************************************************************
 * Project:  DataProxyServer
 * @file     StorageStep.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月21日
 * @note
 * Modify history:
 ******************************************************************************/
#include "RedisStorageStep.hpp"

namespace oss
{

RedisStorageStep::RedisStorageStep(Step* pNextStep)
    : RedisStep(pNextStep)
{
}

RedisStorageStep::RedisStorageStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody, Step* pNextStep)
    : RedisStep(stReqMsgShell, oReqMsgHead, oReqMsgBody, pNextStep)
{
}

RedisStorageStep::~RedisStorageStep()
{
}

bool RedisStorageStep::Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                int iErrno, const std::string& strErrMsg)
{
    LOG4_TRACE("%d: %s", iErrno, strErrMsg.c_str());
    MsgHead oOutMsgHead = oInMsgHead;
    MsgBody oOutMsgBody;
    DataMem::MemRsp oRsp;
    oRsp.set_err_no(iErrno);
    oRsp.set_err_msg(strErrMsg);
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!SendTo(stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("send to MsgShell(fd %d, seq %u) error!", stMsgShell.iFd, stMsgShell.ulSeq);
        return(false);
    }
    return(true);
}

bool RedisStorageStep::Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                const DataMem::MemRsp& oRsp)
{
    LOG4_TRACE("%d: %s", oRsp.err_no(), oRsp.err_msg().c_str());
    if (0 == oRsp.err_no())
    {
        LOG4_DEBUG("oRsp:%s",oRsp.DebugString().c_str());
    }
    MsgHead oOutMsgHead = oInMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!SendTo(stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("send to MsgShell(fd %d, seq %u) error!", stMsgShell.iFd, stMsgShell.ulSeq);
        return(false);
    }
    return(true);
}

} /* namespace oss */
