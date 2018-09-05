/*******************************************************************************
 * Project:  CenterServer
 * @file     StepOfflineNode.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2016年9月14日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepOfflineNode.h"

namespace core
{

StepOfflineNode::StepOfflineNode(const net::tagMsgShell &stMsgShell,const MsgHead& oInMsgHead,
                const std::string& sOfflineNodeIdentify,const server::offline_node_ack &oOfflineNodeAck)
{
    m_stMsgShell = stMsgShell;
    m_oInMsgHead = oInMsgHead;
    m_sOfflineNodeIdentify = sOfflineNodeIdentify;
    m_iTimeOut = 0;
    m_oOfflineNodeAck = oOfflineNodeAck;
}

StepOfflineNode::~StepOfflineNode()
{
}

net::E_CMD_STATUS StepOfflineNode::Emit(int iErrno,
                const std::string &strErrMsg, const std::string &strErrShow)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgHead.set_cmd(net::CMD_REQ_NODE_STOP);
    oOutMsgHead.set_seq(GetSequence());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    bool bRet = GetLabor()->SendTo(m_sOfflineNodeIdentify, oOutMsgHead,
                    oOutMsgBody);
    if (bRet)
    {
        LOG4_DEBUG("StepOfflineNode notify sOfflineNodeIdentify(%s) send ok,seq(%u)",
                        m_sOfflineNodeIdentify.c_str(),GetSequence());
    }
    else
    {
        LOG4_WARN("StepOfflineNode notify sOfflineNodeIdentify(%s) send failed,seq(%u)",
                        m_sOfflineNodeIdentify.c_str(),GetSequence());
    }
    return net::STATUS_CMD_RUNNING;
}

net::E_CMD_STATUS StepOfflineNode::Callback(const net::tagMsgShell &stMsgShell,
                const MsgHead &oInMsgHead, const MsgBody &oInMsgBody,
                void *data)
{
    LOG4CPLUS_DEBUG_FMT(GetLogger(), __FUNCTION__);
    if (oInMsgHead.cmd() == net::CMD_RSP_SYS_ERROR)
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(), "CMD_RSP_SYS_ERROR");
        Response(ERR_SERVERINFO);
        return Response(ERR_PARSE_PACK_ERROR);
    }
    OrdinaryResponse oRes;
    if (!oRes.ParseFromString(oInMsgBody.body()))
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"failed to parse OrdinaryResponse oRes!");
        Response(ERR_SERVERINFO);
        return(net::STATUS_CMD_FAULT);
    }
    if(oRes.err_no())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"error %d: %s!", oRes.err_no(), oRes.err_msg().c_str());
        Response(ERR_SERVERINFO);
        return(net::STATUS_CMD_FAULT);
    }
    LOG4_DEBUG("StepOfflineNode::Callback Response");
    Response(ERR_OK);
    return net::STATUS_CMD_COMPLETED;
}

net::E_CMD_STATUS StepOfflineNode::Timeout()
{
    ++m_iTimeOut;
    if(m_iTimeOut < 5)
    {
        return net::STATUS_CMD_RUNNING;
    }
    LOG4CPLUS_ERROR_FMT(GetLogger(), "%s()", __FUNCTION__);
    Response(core::ERR_ASYNC_TIMEOUT);
    return net::STATUS_CMD_FAULT;
}

net::E_CMD_STATUS StepOfflineNode::Response(int iErrno)
{
    /*
    message offline_node_ack
    {
        common.errorinfo error = 1;//错误码以及错误描述信息
        string inner_ip = 2;//指定节点ip
        uint32 inner_port = 3;//指定节点端口
        uint32 offline = 4;//挂起节点路由:0，关闭节点:1
    }
     * */
    LOG4_DEBUG( "CmdOfflineNode::Response iErrno(%d)",iErrno);
    MsgHead oOutMsgHead;
    server::errorinfo* pError = new server::errorinfo();
    pError->set_error_code(server_err_code(iErrno));
    pError->set_error_info(server_err_msg(iErrno));
    pError->set_error_client_show(server_err_msg(iErrno));
    m_oOfflineNodeAck.set_allocated_error(pError);
    oOutMsgHead.set_cmd(m_oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oInMsgHead.seq());
    if (!SendToClient(m_stMsgShell, oOutMsgHead, m_oOfflineNodeAck))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stMsgShell.iFd, m_stMsgShell.ulSeq);
    }
    return net::STATUS_CMD_COMPLETED;
}

} /* namespace core */
