/*******************************************************************************
 * Project:  DataProxyServer
 * @file     DbStorageStep.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月21日
 * @note
 * Modify history:
 ******************************************************************************/
#include "DbStorageStep.hpp"

namespace oss
{

DbStorageStep::DbStorageStep(Step* pNextStep)
    : Step(pNextStep)
{
}

DbStorageStep::DbStorageStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody, Step* pNextStep)
    : Step(stReqMsgShell, oReqMsgHead, oReqMsgBody, pNextStep)
{
}

DbStorageStep::~DbStorageStep()
{
}

bool DbStorageStep::Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                int iErrno, const std::string& strErrMsg)
{
    LOG4_TRACE("error %d: %s", iErrno, strErrMsg.c_str());
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

bool DbStorageStep::Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                const DataMem::MemRsp& oRsp)
{
    LOG4_TRACE("%d: %s", oRsp.err_no(), oRsp.err_msg().c_str());
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
