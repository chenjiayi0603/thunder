/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdOnlineNode.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2016年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "json/CJsonObject.hpp"
#include "CmdOnlineNode.hpp"
#include "StepRestartWorkers.h"

#ifdef __cplusplus
extern "C"
{
#endif

net::Cmd* create()
{
    net::Cmd* pCmd = new starshiplib::CmdOnlineNode();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace core
{

CmdOnlineNode::CmdOnlineNode()
                :pSess(NULL),boInit(false)
{
}
CmdOnlineNode::~CmdOnlineNode()
{
}

bool CmdOnlineNode::Init()
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

bool CmdOnlineNode::AnyMessage(const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	m_stMsgShell = stMsgShell;
    m_oInMsgHead = oInMsgHead;
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
		return(ERR_REQ_MISS_PARAM);
	}
    //对应的节点信息.sOnlineNodeIdentify(IP:端口)
    char sOnlineNodeIdentify[32];
    snprintf(sOnlineNodeIdentify,sizeof(sOnlineNodeIdentify),"%s:%u",m_oOnlineNodeReq.inner_ip().c_str(),m_oOnlineNodeReq.inner_port());
    if(!pSess->HasIdentifyAuthority(sOnlineNodeIdentify))
    {
        LOG4_DEBUG("HasAuthority none(%s)",sOnlineNodeIdentify);
        return false;
    }
    if (eOnlineFlag_restore_routes == m_oOnlineNodeReq.online())//恢复节点路由:0
    {
        int nRet = pSess->OnlineNode(sOnlineNodeIdentify);
        if(nRet)
        {
            LOG4_WARN("failed to sOnlineNodeIdentify(%s)",sOnlineNodeIdentify);
            return Response(nRet);
        }
        return Response(nRet);
    }
    else if(eOnlineFlag_restart_workers == m_oOnlineNodeReq.online())//重启工作者:1
    {
        int nRet = pSess->CanOnlineNode(sOnlineNodeIdentify);//是否正常节点，是有挂起节点才能被重启工作者
        if (ERR_SERVER_SELF_ONLINE == nRet)
        {
            LOG4_INFO("center worker exit");
            Response(ERR_OK);
            exit(0);
            return true;
        }
        else if (nRet)
        {
            LOG4_WARN("can't OnlineNode sOnlineNodeIdentify(%s)!",
                            sOnlineNodeIdentify);
            return Response(nRet);
        }
        //重启目标节点工作者
        nRet = SendRestartWorkersToTarget(sOnlineNodeIdentify);
        if(nRet)
        {
            LOG4_WARN("failed to SendRestartWorkersToTarget(%s)",sOnlineNodeIdentify);
            return Response(nRet);
        }//成功发送出去后异步回应
        return true;
    }
    else
    {
        LOG4_DEBUG("CmdOnlineNode online(%d) error",m_oOnlineNodeReq.online());
        return Response(ERR_REQ_MISS_PARAM);
    }
}

//发送重启工作者通知到目标服务
int CmdOnlineNode::SendRestartWorkersToTarget(const std::string& sOnlineNodeIdentify)
{
    StepRestartWorkers *pStep = new StepRestartWorkers(m_stMsgShell,m_oInMsgHead,sOnlineNodeIdentify,m_oOnlineNodeAck);
    if(NULL == pStep)
    {
        LOG4_WARN("new StepRestartWorkers failed");
        return ERR_SERVERINFO;
    }
    if(!RegisterCallback(pStep))
    {
        LOG4_WARN("RegisterCallback(pStep) failed");
        delete pStep;//析构函数会回收pStep内存
        pStep = NULL;
        return ERR_SERVERINFO;
    }
    if(net::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return ERR_SERVERINFO;
    }
    return (ERR_OK);
}

bool CmdOnlineNode::parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo)
{
    /*
    message online_node_req
    {
        string inner_ip = 1;//指定修改节点ip
        uint32 inner_port = 2;//指定修改节点端口
        uint32 online= 3;//恢复节点路由:0,重启工作者:1(非中心节点只有挂起的节点才能重启工作者)
    }
     * */
    m_oOnlineNodeReq.Clear();
    if (!ParseFromMsg(oInMsgBody,m_oOnlineNodeReq))
    {
        LOG4_ERROR("%s() ParseFromMsg(oInMsgBody,m_oOnlineNodeReq) failed!",__FUNCTION__);
        return(false);
    }
    m_oOnlineNodeAck.set_inner_ip(m_oOnlineNodeReq.inner_ip());
    m_oOnlineNodeAck.set_inner_port(m_oOnlineNodeReq.inner_port());
    m_oOnlineNodeAck.set_online(m_oOnlineNodeReq.online());
    LOG4_DEBUG("%s() m_oOnlineNodeReq(%s)",m_oOnlineNodeReq.DebugString().c_str());
    return(true);
}

bool CmdOnlineNode::Response(int iErrno)
{
    /*
            服务器节点上线响应
    message online_node_ack
    {
        common.errorinfo error = 1;//错误码以及错误描述信息
        string inner_ip = 2;//指定修改节点ip
        uint32 inner_port = 3;//指定修改节点端口
        uint32 offline = 4;//恢复节点路由:0,重启工作者:1
    }
     * */
    LOG4_DEBUG("CmdOfflineNode::Response iErrno(%d)",iErrno);
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
	return (true);
}


} /* namespace core */
