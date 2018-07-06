/*******************************************************************************
 * Project:  CenterServer
 * @file     StepRestartWorkers.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2016年9月14日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepRestartWorkers.h"

namespace starshiplib
{

StepRestartWorkers::StepRestartWorkers(const oss::tagMsgShell &stMsgShell,const MsgHead& oInMsgHead,
                const std::string& sOnlineNodeIdentify,const server::online_node_ack &oOnlineNodeAck)
{
    m_stMsgShell = stMsgShell;
    m_oInMsgHead = oInMsgHead;
    m_sOnlineNodeIdentify = sOnlineNodeIdentify;
    m_iTimeOut = 0;
    m_oOnlineNodeAck = oOnlineNodeAck;
}

StepRestartWorkers::~StepRestartWorkers()
{
}

oss::E_CMD_STATUS StepRestartWorkers::Emit(int iErrno,
                const std::string &strErrMsg, const std::string &strErrShow)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgHead.set_cmd(oss::CMD_REQ_NODE_RESTART_WORKERS);
    oOutMsgHead.set_seq(GetSequence());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    bool bRet = GetLabor()->SendTo(m_sOnlineNodeIdentify, oOutMsgHead,
                                oOutMsgBody);
    if (bRet)
    {
        LOG4_DEBUG("StepRestartWorkers notify m_sOnlineNodeIdentify(%s) send ok,seq(%u)",
                        m_sOnlineNodeIdentify.c_str(),GetSequence());
    }
    else
    {
        LOG4_WARN("StepRestartWorkers notify m_sOnlineNodeIdentify(%s) send failed,seq(%u)",
                        m_sOnlineNodeIdentify.c_str(),GetSequence());
    }
    return oss::STATUS_CMD_RUNNING;
}

oss::E_CMD_STATUS StepRestartWorkers::Callback(const oss::tagMsgShell &stMsgShell,
                const MsgHead &oInMsgHead, const MsgBody &oInMsgBody,
                void *data)
{
    LOG4CPLUS_DEBUG_FMT(GetLogger(), __FUNCTION__);
    if (oInMsgHead.cmd() == oss::CMD_RSP_SYS_ERROR)
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "CMD_RSP_SYS_ERROR");
        Response(ERR_SERVERINFO);
        return Response(starshiplib::ERR_PARSE_PACK_ERROR);
    }
    OrdinaryResponse oRes;
    if (!oRes.ParseFromString(oInMsgBody.body()))
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to parse OrdinaryResponse oRes!");
        Response(ERR_SERVERINFO);
        return(oss::STATUS_CMD_FAULT);
    }
    if(oRes.err_no())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"error %d: %s!", oRes.err_no(), oRes.err_msg().c_str());
        Response(ERR_SERVERINFO);
        return(oss::STATUS_CMD_FAULT);
    }
    LOG4_DEBUG("StepRestartWorkers::Callback Response");
    Response(ERR_OK);
    return oss::STATUS_CMD_COMPLETED;
}

oss::E_CMD_STATUS StepRestartWorkers::Timeout()
{
    ++m_iTimeOut;
    if(m_iTimeOut < 5)
    {
        return oss::STATUS_CMD_RUNNING;
    }
    LOG4CPLUS_ERROR_FMT(GetLogger(), "%s()", __FUNCTION__);
    Response(ERR_ASYNC_TIMEOUT);
    return oss::STATUS_CMD_FAULT;
}

oss::E_CMD_STATUS StepRestartWorkers::Response(int iErrno)
{
    /*
                服务器节点上线响应
    message online_node_ack
    {
        server.errorinfo error = 1;//错误码以及错误描述信息
        string inner_ip = 2;//指定修改节点ip
        uint32 inner_port = 3;//指定修改节点端口
        uint32 offline = 4;//恢复节点路由:0,重启工作者:1
    }
     * */
    LOG4_DEBUG("%s() StepRestartWorkers::Response iErrno(%d)",iErrno);
    MsgHead oOutMsgHead;
    server::errorinfo* pError = new server::errorinfo();
    pError->set_error_code(server_err_code(iErrno));
    pError->set_error_info(server_err_msg(iErrno));
    pError->set_error_client_show(server_err_msg(iErrno));
    m_oOnlineNodeAck.set_allocated_error(pError);
    oOutMsgHead.set_cmd(m_oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oInMsgHead.seq());
    if (!SendToClient(m_stMsgShell, oOutMsgHead, m_oOnlineNodeAck))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stMsgShell.iFd, m_stMsgShell.ulSeq);
    }
    return oss::STATUS_CMD_COMPLETED;
}

} /* namespace starshiplib */
