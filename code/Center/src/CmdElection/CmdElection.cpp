/*******************************************************************************
 * Project:  Center
 * @file     CmdElection.cpp
 * @brief 
 * @author   cjy
 * @date:    2019-1-6
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdElection.hpp"
#include "coor.pb.h"

MUDULE_CREATE(coor::CmdElection);

namespace coor
{

bool CmdElection::Init()
{
	if (nullptr == m_pSessionOnlineNodes)
	{
		m_pSessionOnlineNodes = GetSessionOnlineNodes();
		if (nullptr == m_pSessionOnlineNodes)
		{
			LOG4_ERROR("no session node found!");
			return false;
		}
	}
    return(true);
}

bool CmdElection::AnyMessage(const net::tagMsgShell& stMsgShell,const MsgHead& oMsgHead,const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s() Election oMsgHead.cmd:%u)", __FUNCTION__, oMsgHead.cmd());
    Election oElection;
	GetLabor()->SendToClient(stMsgShell,oMsgHead,std::string());
    if (oElection.ParseFromString(oMsgBody.body()))
    {
        LOG4_INFO("Election strNodeIdentify(%s)!",GetLabor()->GetConnectIdentify(stMsgShell).c_str());
        m_pSessionOnlineNodes->AddCenterBeat(GetLabor()->GetConnectIdentify(stMsgShell), oElection);
        return(true);
    }
    else
    {
        LOG4_ERROR("failed to parse election info from MsgBody.data()!");
        return(false);
    }
}

} /* namespace coor */
