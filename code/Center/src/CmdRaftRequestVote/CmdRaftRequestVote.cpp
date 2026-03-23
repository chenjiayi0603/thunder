/*******************************************************************************
 * @file CmdRaftRequestVote.cpp
 * @brief CMD 43 — Raft RequestVote（独立插件）
 ******************************************************************************/
#include "CmdRaftRequestVote.hpp"
#include "cmd/CW.hpp"

MUDULE_CREATE(coor::CmdRaftRequestVote);

namespace coor
{

bool CmdRaftRequestVote::Init()
{
    if (!m_pSessionRaft)
    {
        m_pSessionRaft = GetSessionRaftCluster();
        if (!m_pSessionRaft)
        {
            LOG4_ERROR("no SessionRaftCluster");
            return false;
        }
    }
    return true;
}

bool CmdRaftRequestVote::AnyMessage(const net::tagMsgShell &stMsgShell, const MsgHead &oMsgHead, const MsgBody &oMsgBody)
{
    LOG4_TRACE("%s() cmd:%u", __FUNCTION__, oMsgHead.cmd());
    RaftRequestVote req;
    if (!req.ParseFromString(oMsgBody.body()))
    {
        LOG4_ERROR("failed to parse RaftRequestVote");
        return GetLabor()->SendToClient(stMsgShell, oMsgHead, std::string());
    }
    RaftRequestVoteRsp rsp;
    m_pSessionRaft->HandleRaftRequestVote(GetLabor()->GetConnectIdentify(stMsgShell), req, &rsp);
    return GetLabor()->SendToClient(stMsgShell, oMsgHead, rsp.SerializeAsString());
}

} // namespace coor
