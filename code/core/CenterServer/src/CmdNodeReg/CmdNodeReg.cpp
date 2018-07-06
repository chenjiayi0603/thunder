/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdNodeReg.cpp
 * @brief 
 * @author   cjy
 * @date:    2015年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "json/CJsonObject.hpp"
#include "CmdNodeReg.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

oss::Cmd* create()
{
    oss::Cmd* pCmd = new starshiplib::CmdNodeReg();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace starshiplib
{

CmdNodeReg::CmdNodeReg()
                :pSess(NULL),boInit(false)
{
}
CmdNodeReg::~CmdNodeReg()
{
}

bool CmdNodeReg::Init()
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

bool CmdNodeReg::AnyMessage(const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    loss::CJsonObject jParseObj;
    if (!jParseObj.Parse(oInMsgBody.body()))
    {
        LOG4_DEBUG("failed to parse json body");
        return Response(stMsgShell,oInMsgHead,ERR_BODY_JSON);
    }
    LOG4_DEBUG("CmdNodeReg jsonbuf[%s] Parse is ok",
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
        LOG4_ERROR("CmdNodeReg msg jsonbuf[%s] is wrong,error code(%d)",
                        oInMsgBody.body().c_str(),regNodeRet);
        return Response(stMsgShell,oInMsgHead,ERR_SERVERINFO);
    }
    LOG4_DEBUG("nodeinfo.getNodeKey(%s),stMsgShell(%d,%u)",nodeinfo.getNodeKey().c_str(),stMsgShell.iFd,stMsgShell.ulSeq);
//    GetLabor()->AddMsgShell(nodeinfo.getNodeKey(), stMsgShell);//加入到该节点的路由信息
    return true;
}

bool CmdNodeReg::Response(const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,int iRet,int node_id)
{
    /*
     * 返回结构
     * {
     *  "errcode":0,
     *  "node_id":1
     * }
     * */
    LOG4_DEBUG( "CmdNodeReg::Response iRet(%d),node_id(%d)",iRet,node_id);
    if(iRet)
    {
        MsgHead oOutMsgHead;
        MsgBody oOutMsgBody;
        oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
        oOutMsgHead.set_seq(oInMsgHead.seq());
        loss::CJsonObject jObjReturn;
        jObjReturn.Add("errcode", iRet);
        jObjReturn.Add("node_id", iRet ? 0 :node_id);
        oOutMsgBody.set_body(jObjReturn.ToString());
        oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
        GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
    }
    return (iRet ? false : true);
}


} /* namespace starshiplib */
