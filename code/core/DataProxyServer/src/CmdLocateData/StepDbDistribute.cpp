/*******************************************************************************
 * Project:  DataProxyServer
 * @file     StepDbDistribute.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年4月18日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepDbDistribute.hpp"

namespace oss
{

StepDbDistribute::StepDbDistribute(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,
                const loss::CJsonObject* pRedisNode)
    : Step(stMsgShell, oInMsgHead, oInMsgBody), m_oRedisNode(pRedisNode)
{
}

StepDbDistribute::~StepDbDistribute()
{
}

E_CMD_STATUS StepDbDistribute::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    MsgHead oOutMsgHead;
    oOutMsgHead.set_cmd(m_oReqMsgHead.cmd());
    oOutMsgHead.set_seq(GetSequence());
    oOutMsgHead.set_msgbody_len(m_oReqMsgBody.ByteSize());
    if (!SendToNext("DBAGENT_R", oOutMsgHead, m_oReqMsgBody))
    {
        LOG4_ERROR("SendToNext(\"DBAGENT_R\") error!");
        Response(m_stReqMsgShell, m_oReqMsgHead, ERR_DATA_TRANSFER, "SendToNext(\"DBAGENT_R\") error!");
        return(STATUS_CMD_FAULT);
    }
    return(STATUS_CMD_RUNNING);
}

E_CMD_STATUS StepDbDistribute::Callback(const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, void* data)
{
    LOG4_DEBUG("%s()", __FUNCTION__);
    MsgHead oOutMsgHead = m_oReqMsgHead;
    MsgBody oOutMsgBody;
    loss::CJsonObject oRspJson;
    if (!oRspJson.Parse(oInMsgBody.body()))
    {
        LOG4_ERROR("oRspJson.Parse failed!");
    }
    if (m_oRedisNode.IsEmpty())
    {
        oOutMsgBody.set_body(oInMsgBody.body());
    }
    else
    {
        oRspJson.Add("redis_node", m_oRedisNode);
        oOutMsgBody.set_body(oRspJson.ToFormattedString());
    }
    oOutMsgHead.set_cmd(m_oReqMsgHead.cmd() + 1);
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!SendTo(m_stReqMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("send to MsgShell(fd %d, seq %u) error!", m_stReqMsgShell.iFd, m_stReqMsgShell.ulSeq);
        return(STATUS_CMD_FAULT);
    }
    return(STATUS_CMD_COMPLETED);
}

E_CMD_STATUS StepDbDistribute::Timeout()
{
    Response(m_stReqMsgShell, m_oReqMsgHead, ERR_TIMEOUT, "\"DBAGENT_R\" timeout!");
    return(STATUS_CMD_FAULT);
}

bool StepDbDistribute::Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                int iErrno, const std::string& strErrMsg)
{
    LOG4_DEBUG("%d: %s", iErrno, strErrMsg.c_str());
    MsgHead oOutMsgHead = oInMsgHead;
    MsgBody oOutMsgBody;
    loss::CJsonObject oRspJson;
    oRspJson.Add("code", iErrno);
    oRspJson.Add("msg", strErrMsg);
    oOutMsgBody.set_body(oRspJson.ToFormattedString());
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("send to MsgShell(fd %d, seq %u) error!", stMsgShell.iFd, stMsgShell.ulSeq);
        return(false);
    }
    return(true);
}

} /* namespace oss */
