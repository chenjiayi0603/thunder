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

    net::Cmd* create()
    {
        net::Cmd* pCmd = new core::CmdNodeDisconnect();
        return(pCmd);
    }

#ifdef __cplusplus
}
#endif

namespace core
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
                const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    pSess->DelNode(oInMsgBody.body());//delNodeIdentify
    return true;
}




} /* namespace core */
