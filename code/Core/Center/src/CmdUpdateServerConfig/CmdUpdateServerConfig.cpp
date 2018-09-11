/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdUpdateServerConfig.cpp
 * @brief 
 * @author   cjy
 * @date:    2015年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "util/json/CJsonObject.hpp"
#include "CmdUpdateServerConfig.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

net::Cmd* create()
{
    net::Cmd* pCmd = new core::CmdUpdateServerConfig();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace core
{

CmdUpdateServerConfig::CmdUpdateServerConfig()
                :pSess(NULL),boInit(false)
{
}
CmdUpdateServerConfig::~CmdUpdateServerConfig()
{
}

bool CmdUpdateServerConfig::Init()
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

bool CmdUpdateServerConfig::AnyMessage(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
	LOG4_TRACE("%s()", __FUNCTION__);
    int nRet = CheckReqMsg(oInMsgBody);
    if (nRet > 0 && pSess->IsMaster())
	{
		Response(nRet,stMsgShell,oInMsgHead);
		return false;
	}
    nRet = pSess->UpdateServerConfig(m_oUpdateServerConfigReq,m_oUpdateServerConfigAck);
    return Response(nRet,stMsgShell,oInMsgHead);
}

int CmdUpdateServerConfig::CheckReqMsg(const MsgBody& oInMsgBody)
{
	if(oInMsgBody.session_id() == 0)//session_id   appid << 32 | userid
	{
		LOG4_ERROR("%s() session_id not exist",__FUNCTION__);
		return(ERR_INVALID_PROTOCOL);
	}
	if(oInMsgBody.additional().size() == 0)
	{
		LOG4_ERROR("%s() additional not exist",__FUNCTION__);
		return(ERR_INVALID_PROTOCOL);
	}
	server::user_basic basicInfo;
	if(!basicInfo.ParseFromString(oInMsgBody.additional()))
	{
		LOG4_ERROR("%s() Parse additional failed",__FUNCTION__);
		return(ERR_INVALID_PROTOCOL);
	}
	if(basicInfo.user_type() != eUserType_supermanager)
	{
		LOG4_ERROR("%s() no operate right",__FUNCTION__);
		return(ERR_NO_OPERATION_PERMISSIONS);
	}
	if(!parseMsg(oInMsgBody,basicInfo))
	{
		LOG4_ERROR("parseHttpMsg failed:%d:%s", server_err_code(ERR_MSG_BODY_DECODE), server_err_msg(ERR_MSG_BODY_DECODE));
		return(ERR_REQ_MISS_PARAM);
	}
	return(ERR_OK);
}

bool CmdUpdateServerConfig::parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo)
{
    /*
            更新服务器配置请求
    message update_server_config_req
    {
        node_config config =1;//服务器配置
        string inner_ip = 2;//指定修改节点ip（可选）
        uint32 inner_port = 3;//指定修改节点端口（可选）
    }
     * */
    m_oUpdateServerConfigReq.Clear();
    if (!ParseFromMsg(oInMsgBody,m_oUpdateServerConfigReq))
    {
        LOG4_ERROR("%s() ParseFromMsg(oInMsgBody,m_oOnlineNodeReq) failed!",__FUNCTION__);
        return(false);
    }
    m_oUpdateServerConfigAck.set_inner_ip(m_oUpdateServerConfigReq.inner_ip());
    m_oUpdateServerConfigAck.set_inner_port(m_oUpdateServerConfigReq.inner_port());
    m_oUpdateServerConfigAck.mutable_config()->CopyFrom(m_oUpdateServerConfigReq.config());
    LOG4_DEBUG("%s() m_oUpdateServerConfigReq(%s)",m_oUpdateServerConfigReq.DebugString().c_str());
    return(true);
}

bool CmdUpdateServerConfig::Response(int iErrno,const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead)
{
	if (ERR_SERVER_CENTER_NO_OPERATION == iErrno)
	{//只有ERR_SERVER_CENTER_NO_OPERATION是指定中心节点操作后指定中心节点返回
		LOG4_DEBUG("no right to operate,no need to response");
		return false;
	}
	if (!pSess->IsMaster())
	{
		LOG4_DEBUG("is not master,no need to response");
		return false;
	}
    /*
                服务器节点上线响应
    message update_server_config_ack
    {
        server.errorinfo error = 1;//错误码以及错误描述信息
        node_config config =2;//服务器配置
        string inner_ip = 2;//指定修改节点ip（可选）
        uint32 inner_port = 3;//指定修改节点端口（可选）
    }
     * */
    LOG4_DEBUG( "CmdOfflineNode::Response iErrno(%d)",iErrno);
    MsgHead oOutMsgHead;
    server::errorinfo* pError = new server::errorinfo();
    pError->set_error_code(server_err_code(iErrno));
    pError->set_error_info(server_err_msg(iErrno));
    pError->set_error_client_show(server_err_msg(iErrno));
    m_oUpdateServerConfigAck.set_allocated_error(pError);
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(oInMsgHead.seq());
    if (!SendToClient(stMsgShell, oOutMsgHead, m_oUpdateServerConfigAck))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
        		stMsgShell.iFd, stMsgShell.ulSeq);
    }
    return (true);
}


} /* namespace core */
