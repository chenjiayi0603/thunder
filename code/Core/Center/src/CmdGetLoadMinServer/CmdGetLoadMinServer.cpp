/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdGetLoadMinServer.cpp
 * @brief 
 * @author   chenjiayi
 * @date:    2015年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include "json/CJsonObject.hpp"
#include "../NodeSession.h"
#include "CmdGetLoadMinServer.hpp"

using namespace std;

#ifdef __cplusplus
extern "C"
{
#endif

net::Cmd* create()
{
    net::Cmd* pCmd = new starshiplib::CmdGetLoadMinServer();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace core
{

CmdGetLoadMinServer::CmdGetLoadMinServer()
                : pSess(NULL),boInit(false)
{
}

CmdGetLoadMinServer::~CmdGetLoadMinServer()
{
}

bool CmdGetLoadMinServer::Init()
{
    if (boInit)
    {
        return true;
    }
    pSess = GetNodeSession(GetLabor(),GetConfigPath(),true);//本cmd重新加载配置
    if(!pSess)
    {
        LOG4_ERROR("failed to get GetNodeSession");
        return false;
    }
    boInit = true;
    return true;
}

bool CmdGetLoadMinServer::AnyMessage(const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
//请求格式
// {
//    "servertype":"ACCESS"
// }
//返回格式
// {
//     "errcode":0,
//     "outerport":1000,
//     "outerip":"192.168.18.22",
//     "innerport":2000,
//     "innerip":"192.168.18.22"
// }
    NodeLoadStatus nodeLoadStatus;
    int nRet = pSess->GetLoadMinNode(oInMsgHead,oInMsgBody,nodeLoadStatus);
    return Response(stMsgShell,oInMsgHead,nodeLoadStatus,nRet);
}

bool CmdGetLoadMinServer::Response(const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,const NodeLoadStatus& nodeLoadStatus,int iRet)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(oInMsgHead.seq());
    util::CJsonObject jObjReturn;
    jObjReturn.Add("outerport", nodeLoadStatus.outerport);
    jObjReturn.Add("outerip", nodeLoadStatus.outerip);
    jObjReturn.Add("innerport", nodeLoadStatus.innerport);
    jObjReturn.Add("innerip", nodeLoadStatus.innerip);
    jObjReturn.Add("errcode", iRet);
    LOG4_DEBUG("CmdGetLoadMinServer return json[%s]",
                                jObjReturn.ToString().c_str());
    oOutMsgBody.set_body(jObjReturn.ToString());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
    return (iRet ? false : true);
}


} /* namespace core */
