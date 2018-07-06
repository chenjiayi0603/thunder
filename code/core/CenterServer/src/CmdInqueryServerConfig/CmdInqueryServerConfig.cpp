/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdInqueryServerConfig.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2016年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "json/CJsonObject.hpp"
#include "CmdInqueryServerConfig.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

oss::Cmd* create()
{
    oss::Cmd* pCmd = new starshiplib::CmdInqueryServerConfig();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace starshiplib
{

CmdInqueryServerConfig::CmdInqueryServerConfig()
                :boInit(false)
{
}
CmdInqueryServerConfig::~CmdInqueryServerConfig()
{
}

bool CmdInqueryServerConfig::Init()
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

bool CmdInqueryServerConfig::AnyMessage(const oss::tagMsgShell& stMsgShell,
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
		return(ERR_INVALID_PROTOCOL);
	}
    nRet = pSess->CheckServerConfig(m_oInqueryServerConfigReq,m_oInqueryServerConfigAck);
    return Response(nRet);
}

bool CmdInqueryServerConfig::parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo)
{
    /*
    message inquery_server_config_req
    {
        string node_type= 1;//节点类型，如LOGIC
        uint32 config_type = 2;//配置类型，0:服务器配置，其他类型为逻辑配置
    }
     * */
    m_oInqueryServerConfigReq.Clear();
    if (!ParseFromMsg(oInMsgBody,m_oInqueryServerConfigReq))
    {
        LOG4_ERROR("%s() ParseFromMsg(oInMsgBody,m_oInqueryServerConfigReq) failed!",__FUNCTION__);
        return(false);
    }
    LOG4_DEBUG("%s() m_oInqueryServerConfigReq(%s)",m_oInqueryServerConfigReq.DebugString().c_str());
    return(true);
}

bool CmdInqueryServerConfig::Response(int iErrno)
{
	/*
                        查询服务器配置响应
        message inquery_server_config_ack
        {
            server.errorinfo error = 1;//错误码以及错误描述信息
            node_config config =2;//服务器配置
        }
	 * */
	LOG4_DEBUG( "CmdInqueryServerConfig::Response iErrno(%d)",iErrno);
	MsgHead oOutMsgHead;
    server::errorinfo* pError = new server::errorinfo();
    pError->set_error_code(server_err_code(iErrno));
    pError->set_error_info(server_err_msg(iErrno));
    pError->set_error_client_show(server_err_msg(iErrno));
    m_oInqueryServerConfigAck.set_allocated_error(pError);
    oOutMsgHead.set_cmd(m_oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oInMsgHead.seq());
    if (!SendToClient(m_stMsgShell, oOutMsgHead, m_oInqueryServerConfigAck))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stMsgShell.iFd, m_stMsgShell.ulSeq);
    }
	return (true);
}


} /* namespace starshiplib */
