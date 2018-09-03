/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdHttpCheckServerLoad.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2016年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "json/CJsonObject.hpp"
#include "CmdCheckServerLoad.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

net::Cmd* create()
{
    net::Cmd* pCmd = new starshiplib::CmdCheckServerLoad();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace core
{

CmdCheckServerLoad::CmdCheckServerLoad()
                :pSess(NULL),boInit(false)
{
}
CmdCheckServerLoad::~CmdCheckServerLoad()
{
}

bool CmdCheckServerLoad::Init()
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

bool CmdCheckServerLoad::AnyMessage(const net::tagMsgShell& stMsgShell,
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
        Response(ERR_REQ_MISS_PARAM);
        return(false);
    }
    nRet = pSess->CheckServerLoad(m_oCheckServerLoadReq,m_oCheckServerLoadAck);
    return Response(nRet);
}

bool CmdCheckServerLoad::parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo)
{
    /*
    message check_server_load_req
    {
        string inner_ip = 1;//指定节点ip
        uint32 inner_port = 2;//指定节点端口
    }
     * */
    m_oCheckServerLoadReq.Clear();
    if (!ParseFromMsg(oInMsgBody,m_oCheckServerLoadReq))
    {
        LOG4_ERROR("%s() ParseFromMsg(oInMsgBody,m_oCheckServerLoadReq) failed!",__FUNCTION__);
        return(false);
    }
    LOG4_DEBUG("%s() m_oCheckServerLoadReq(%s)",m_oCheckServerLoadReq.DebugString().c_str());
    return(true);
}

bool CmdCheckServerLoad::Response(int iErrno)
{
	/*
	  查询服务器负载响应
    message check_server_load_ack
    {
        common.errorinfo error = 1;//错误码以及错误描述信息
        string inner_ip = 2;//指定修改节点ip
        uint32 inner_port = 3;//指定修改节点端口
        uint32 status = 4;//1:已启动，2:未启动
        uint32 add_up_recv_num = 5;//最近收包数量统计（统计时间为配置时间）
        uint32 add_up_send_num = 6;//最近发包数量统计
        uint32 add_up_recv_byte = 7;//最近收包字节统计
        uint32 add_up_send_byte = 8;//最近发包字节统计
    }
	 * */
	LOG4_DEBUG( "CmdCheckServerLoad::Response iErrno(%d)",iErrno);
	LOG4_DEBUG( "CmdInqueryServerConfig::Response iErrno(%d)",iErrno);
    MsgHead oOutMsgHead;
    server::errorinfo* pError = new server::errorinfo();
    pError->set_error_code(server_err_code(iErrno));
    pError->set_error_info(server_err_msg(iErrno));
    pError->set_error_client_show(server_err_msg(iErrno));
    m_oCheckServerLoadAck.set_allocated_error(pError);
    oOutMsgHead.set_cmd(m_oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oInMsgHead.seq());
    if (!SendToClient(m_stMsgShell, oOutMsgHead, m_oCheckServerLoadAck))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stMsgShell.iFd, m_stMsgShell.ulSeq);
    }
	return (true);
}


} /* namespace core */
