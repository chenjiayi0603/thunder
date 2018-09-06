/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdNodeRegister.cpp
 * @brief 
 * @author   cjy
 * @date:    2015年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "util/json/CJsonObject.hpp"
#include "CmdNodeRegister.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

net::Cmd* create()
{
    net::Cmd* pCmd = new core::CmdNodeRegister();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace core
{

CmdNodeRegister::CmdNodeRegister()
                :pSess(NULL),boInit(false)
{
}
CmdNodeRegister::~CmdNodeRegister()
{
}

bool CmdNodeRegister::Init()
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

bool CmdNodeRegister::AnyMessage(const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    util::CJsonObject jParseObj;
    if (!jParseObj.Parse(oInMsgBody.body()))
    {
        LOG4_DEBUG("failed to parse json body");
        return Response(stMsgShell,oInMsgHead,ERR_BODY_JSON);
    }
    LOG4_DEBUG("CmdNodeRegister jsonbuf[%s] Parse is ok",
                                oInMsgBody.body().c_str());
    //解析节点数据
    NodeStatusInfo nodeinfo;
    if (!nodeinfo.pareJsonData(jParseObj))
    {
        LOG4_ERROR("nodeinfo.pareJsonData error");
        return Response(stMsgShell,oInMsgHead,ERR_BODY_JSON);
    }
    if (!pSess->CheckNodeStatus(nodeinfo))
    {
        LOG4_ERROR("CheckNodeStatus error.nodeType(%s)",
                        nodeinfo.nodeType.c_str());
        return Response(stMsgShell,oInMsgHead,ERR_SERVERINFO);
    }
    int regNodeRet = pSess->RegNode(stMsgShell, oInMsgHead,
                    oInMsgBody, nodeinfo);
    if (regNodeRet)//注册失败返回注册应答，否则已在注册函数中发送
    {
        LOG4_ERROR("CmdNodeRegister msg jsonbuf[%s] is wrong,error code(%d)",
                        oInMsgBody.body().c_str(),regNodeRet);
        return Response(stMsgShell,oInMsgHead,ERR_SERVERINFO);
    }
    LOG4_DEBUG("nodeinfo.getNodeKey(%s),stMsgShell(%d,%u)",nodeinfo.getNodeKey().c_str(),stMsgShell.iFd,stMsgShell.ulSeq);
//    GetLabor()->AddMsgShell(nodeinfo.getNodeKey(), stMsgShell);//加入到该节点的路由信息
    return true;
}

bool CmdNodeRegister::Response(const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,int iRet,int node_id)
{
    /*
     * 返回结构
     * {
     *  "errcode":0,
     *  "node_id":1
     * }
     * */
    LOG4_DEBUG( "CmdNodeRegister::Response iRet(%d),node_id(%d)",iRet,node_id);
    if(iRet)
    {
        MsgHead oOutMsgHead;
        MsgBody oOutMsgBody;
        oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
        oOutMsgHead.set_seq(oInMsgHead.seq());
        util::CJsonObject jObjReturn;
        jObjReturn.Add("errcode", iRet);
        jObjReturn.Add("node_id", iRet ? 0 :node_id);
        oOutMsgBody.set_body(jObjReturn.ToString());
        oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
        GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
    }
    return (iRet ? false : true);
}


} /* namespace core */
