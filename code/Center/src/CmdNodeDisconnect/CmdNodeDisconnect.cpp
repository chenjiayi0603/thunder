/*******************************************************************************
 * Project:  Center
 * @file     CmdNodeDisconnect.cpp
 * @brief 
 * @author   Tommy
 * @date:    Feb 14, 2017
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdNodeDisconnect.hpp"

MUDULE_CREATE(coor::CmdNodeDisconnect);

namespace coor
{

bool CmdNodeDisconnect::Init()
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

bool CmdNodeDisconnect::AnyMessage(const net::tagMsgShell& stMsgShell,const MsgHead& oMsgHead,const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s() NodeDisconnect oMsgHead.cmd:%u)", __FUNCTION__, oMsgHead.cmd());
    if (oMsgBody.body().size() > 0)
    {
        LOG4_TRACE("(%s) disconnect, remove from node list.", oMsgBody.body().c_str());//192.168.11.66:16068 disconnect
        m_pSessionOnlineNodes->RemoveNode(oMsgBody.body());
    }
    else
    {
        LOG4_WARN("(%s) disconnect error.", oMsgBody.body().c_str());
    }
    return(true);
}

} /* namespace coor */
