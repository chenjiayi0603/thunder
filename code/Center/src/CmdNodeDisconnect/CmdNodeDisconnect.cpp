/*******************************************************************************
 * Project:  Center
 * @file     CmdNodeDisconnect.cpp
 * @brief 
 * @author   cjy
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
    const std::string &strNodeIdentify = oMsgBody.body();
    LOG4_TRACE("%s() NodeDisconnect oMsgHead.cmd:%u nodeIdentify:%s", __FUNCTION__, oMsgHead.cmd(),strNodeIdentify.c_str());
    if (oMsgBody.body().size() > 0)
    {
        // 仅 Leader 根据断连摘表；Follower 上业务断连不修改副本（权威下线由 Leader 心跳超时/复制收敛）
        if (!m_pSessionOnlineNodes->IsLeadership())
        {
            LOG4_TRACE("%s() NodeDisconnect ignored on non-leader nodeIdentify:%s", __FUNCTION__, strNodeIdentify.c_str());
            return true;
        }
        LOG4_TRACE("(%s) disconnect, remove from node list.", oMsgBody.body().c_str());//192.168.11.66:16068 disconnect
        m_pSessionOnlineNodes->RemoveNode(strNodeIdentify);
    }
    else
    {
        LOG4_WARN("(%s) disconnect error.", oMsgBody.body().c_str());
    }
    return(true);
}

} /* namespace coor */
