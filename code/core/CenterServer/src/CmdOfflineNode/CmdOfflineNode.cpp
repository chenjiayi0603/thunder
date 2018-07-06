/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdOfflineNode.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2016年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "json/CJsonObject.hpp"
#include "CmdOfflineNode.hpp"
#include "StepOfflineNode.h"


#ifdef __cplusplus
extern "C"
{
#endif

oss::Cmd* create()
{
    oss::Cmd* pCmd = new starshiplib::CmdOfflineNode();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace starshiplib
{

CmdOfflineNode::CmdOfflineNode()
                :pSess(NULL),boInit(false)
{
}
CmdOfflineNode::~CmdOfflineNode()
{
}

bool CmdOfflineNode::Init()
{
    if (boInit)
    {
        return true;
    }
    pSess = GetNodeSession(GetLabor(),GetConfigPath(),true);
    if(!pSess)
    {
        LOG4_ERROR("failed to get GetNodeSession");
        return false;
    }
    boInit = true;
    return true;
}

bool CmdOfflineNode::AnyMessage(const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    m_stMsgShell = stMsgShell;
    m_oInMsgHead = oInMsgHead;
    LOG4_TRACE("%s()", __FUNCTION__);
    if(!pSess->IsMaster())
	{
		LOG4_DEBUG("is not master");
		return false;
	}
	server::user_basic basicInfo;
	int nRet = pSess->CheckMgrMsg(oInMsgBody,basicInfo);
	if (nRet > 0)
	{
		return Response(nRet);
	}
    if(!parseMsg(oInMsgBody,basicInfo))
    {
        LOG4_ERROR("parseHttpMsg failed:%d:%s", server_err_code(ERR_MSG_BODY_DECODE), server_err_msg(ERR_MSG_BODY_DECODE));
        if (pSess->IsMaster())
        {
            Response(ERR_REQ_MISS_PARAM);
        }
        return(false);
    }

    char sOfflineNodeIdentify[32];
    snprintf(sOfflineNodeIdentify,sizeof(sOfflineNodeIdentify),"%s:%u",m_oOfflineNodeReq.inner_ip().c_str(),m_oOfflineNodeReq.inner_port());
    if(!pSess->HasIdentifyAuthority(sOfflineNodeIdentify))
    {
        LOG4_DEBUG("HasAuthority none(%s)",sOfflineNodeIdentify);
        return false;
    }
    if(eofflineFlag_close_note != m_oOfflineNodeReq.offline() && eofflineFlag_suspend_routes != m_oOfflineNodeReq.offline())
    {
        LOG4_WARN("%s() invalid offline(%d)",__FUNCTION__,m_oOfflineNodeReq.offline());
        return Response(ERR_REQ_MISS_PARAM);
    }
    nRet = pSess->OfflineNode(sOfflineNodeIdentify);
    if(ERR_SERVER_SELF_OFFLINE == nRet)//下线的是自己
    {
        if(eofflineFlag_close_note == m_oOfflineNodeReq.offline())//关闭节点
        {
            LOG4_WARN("can't shutdown center using web command");
            return Response(ERR_SERVER_CENTER_RESTART_SCRIPT);
        }
        else if (eofflineFlag_suspend_routes == m_oOfflineNodeReq.offline())//挂起路由
        {
            LOG4_WARN("can't suspend center");
            return Response(ERR_SERVER_CENTER_NO_SUSPEND);
        }
    }
    else if(nRet && (ERR_SERVER_NODE_ALREADY_OFFLINE != nRet))//被挂起的依然可以被关闭
    {
        LOG4_WARN("failed to OfflineNode(%s)",sOfflineNodeIdentify);
        return Response(nRet);
    }
    if(eofflineFlag_close_note == m_oOfflineNodeReq.offline())//关闭节点
    {
        nRet = SendOfflineToTarget(sOfflineNodeIdentify);
        if(nRet)
        {
            LOG4_WARN("failed to SendOfflineToTarget(%s)",sOfflineNodeIdentify);
            return Response(nRet);
        }//成功发送出去后异步回应
        return true;
    }
    return Response(nRet);
}


//发送下线通知到目标服务
int CmdOfflineNode::SendOfflineToTarget(const std::string& sOfflineNodeIdentify)
{
    StepOfflineNode *pStep = new StepOfflineNode(m_stMsgShell,m_oInMsgHead,sOfflineNodeIdentify,m_oOfflineNodeAck);
    if(NULL == pStep)
    {
        LOG4_WARN("new SendOfflineToTarget failed");
        return ERR_SERVERINFO;
    }
    if(!RegisterCallback(pStep))
    {
        LOG4_WARN("RegisterCallback(pStep) failed");
        delete pStep;//析构函数会回收pStep内存
        pStep = NULL;
        return ERR_SERVERINFO;
    }
    if(oss::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return ERR_SERVERINFO;
    }
    return (oss::ERR_OK);
}

bool CmdOfflineNode::parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo)
{
    /*
    message offline_node_req
    {
        string inner_ip = 1;//指定修改节点ip
        uint32 inner_port = 2;//指定修改节点端口
        uint32 offline = 3;//挂起节点路由:0，关闭节点:1
    }
     * */
    m_oOfflineNodeReq.Clear();
    if (!ParseFromMsg(oInMsgBody,m_oOfflineNodeReq))
    {
        LOG4_ERROR("%s() ParseFromMsg(oInMsgBody,m_oOfflineNodeReq) failed!",__FUNCTION__);
        return(false);
    }
    m_oOfflineNodeAck.set_inner_ip(m_oOfflineNodeReq.inner_ip());
    m_oOfflineNodeAck.set_inner_port(m_oOfflineNodeReq.inner_port());
    m_oOfflineNodeAck.set_offline(m_oOfflineNodeReq.offline());
    LOG4_DEBUG("%s() m_oOfflineNodeReq(%s)",m_oOfflineNodeReq.DebugString().c_str());
    return(true);
}

bool CmdOfflineNode::Response(int iErrno)
{
	/*
	 服务器节点下线响应
    message offline_node_ack
    {
        server.errorinfo error = 1;//错误码以及错误描述信息
        string inner_ip = 2;//指定修改节点ip
        uint32 inner_port = 3;//指定修改节点端口
        uint32 offline = 4;//挂起节点路由:0，关闭节点:1
    }
	 * */
    LOG4_DEBUG("CmdOfflineNode::Response iErrno(%d)",iErrno);
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
	return (true);
}


} /* namespace starshiplib */
