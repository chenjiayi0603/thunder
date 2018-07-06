/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdServerReport.cpp
 * @brief 
 * @author   cjy
 * @date:    2015年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdServerDataStatusReport.hpp"

using namespace std;

#ifdef __cplusplus
extern "C"
{
#endif

oss::Cmd* create()
{
    oss::Cmd* pCmd = new starshiplib::CmdServerReport();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace starshiplib
{

CmdServerReport::CmdServerReport()
                : pSess(NULL),boInit(false)
{
}

CmdServerReport::~CmdServerReport()
{
}

bool CmdServerReport::Init()
{
    if (boInit)
    {
        return true;
    }
    LOG4_DEBUG("CmdServerReport::Init");
    pSess = GetNodeSession(GetLabor(),GetConfigPath(),true);
    if(!pSess)
    {
        LOG4_ERROR("failed to get GetNodeSession");
        return false;
    }
    boInit = true;
    return true;
}

bool CmdServerReport::AnyMessage(const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    int nRet = pSess->WriteServerDataLoad(stMsgShell,oInMsgHead,oInMsgBody);
    return Response(stMsgShell,oInMsgHead,nRet);
}

bool CmdServerReport::Response(const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,int iRet)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(oInMsgHead.seq());
    loss::CJsonObject jObjReturn;
    jObjReturn.Add("errcode", iRet);
    oOutMsgBody.set_body(jObjReturn.ToString());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
    return (iRet ? false : true);
}

} /* namespace starshiplib */
