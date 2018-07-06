/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdNodeDisconnect.cpp
 * @brief 
 * @author   cjy
 * @date:    2015年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdNodeDisconnect.hpp"

#ifdef __cplusplus
extern "C" {
#endif

    oss::Cmd* create()
    {
        oss::Cmd* pCmd = new starshiplib::CmdNodeDisconnect();
        return(pCmd);
    }

#ifdef __cplusplus
}
#endif

namespace starshiplib
{
CmdNodeDisconnect::CmdNodeDisconnect():pSess(NULL),boInit(false)
{
}

CmdNodeDisconnect::~CmdNodeDisconnect()
{

}
bool CmdNodeDisconnect::Init()
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

bool CmdNodeDisconnect::AnyMessage(
                const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    const std::string& delNodeIdentify = oInMsgBody.body();
    pSess->DelNode(oInMsgHead,oInMsgBody,delNodeIdentify);
    return true;
}




} /* namespace starshiplib */
