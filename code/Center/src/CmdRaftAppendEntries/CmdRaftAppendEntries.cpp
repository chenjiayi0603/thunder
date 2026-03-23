/*******************************************************************************
 * @file CmdRaftAppendEntries.cpp
 * @brief CMD 45 — Raft AppendEntries（独立插件）
 ******************************************************************************/
#include "CmdRaftAppendEntries.hpp"
#include "cmd/CW.hpp"

MUDULE_CREATE(coor::CmdRaftAppendEntries);

namespace coor
{

bool CmdRaftAppendEntries::Init()
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

bool CmdRaftAppendEntries::AnyMessage(const net::tagMsgShell &stMsgShell, const MsgHead &oMsgHead, const MsgBody &oMsgBody)
{
    LOG4_TRACE("%s() cmd:%u", __FUNCTION__, oMsgHead.cmd());
    RaftAppendEntries req;
    if (!req.ParseFromString(oMsgBody.body()))
    {
        LOG4_ERROR("failed to parse RaftAppendEntries");
        return GetLabor()->SendToClient(stMsgShell, oMsgHead, std::string());
    }
    RaftAppendEntriesRsp rsp;
    m_pSessionRaft->HandleRaftAppendEntries(GetLabor()->GetConnectIdentify(stMsgShell), req, &rsp);
    return GetLabor()->SendToClient(stMsgShell, oMsgHead, rsp.SerializeAsString());
}

} // namespace coor
