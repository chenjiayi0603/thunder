/*******************************************************************************
 * Project:  Center
 * @file     CmdNodeReport.cpp
 * @brief
 * @author   cjy
 * @date:    Feb 14, 2017
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdNodeReport.hpp"
#include "SessionRaftCluster.hpp"

MUDULE_CREATE(coor::CmdNodeReport);

namespace coor
{
namespace
{
uint32_t ApplyNodeReportRaftOutcome(SessionRaftCluster *raft, SessionOnlineNodes *nodes, uint16_t unNodeId, const std::string &strNodeIdentify,
                                    const NodeReport &oNodeReport, NodeReportRsp &rsp)
{
    if (unNodeId != 0)
    {
        rsp.set_node_id(unNodeId);
        return 0u;
    }
    rsp.clear_node_id();
    if (!raft || !raft->RaftHasStableLeader())
    {
        LOG4_WARN("NodeReport err=2 nodeIdentify:%s (no stable raft leader)", strNodeIdentify.c_str());
        return 2u;
    }
    if ((!nodes || !nodes->IsLeadership()) && oNodeReport.node_id() == 0)
    {
        LOG4_WARN("NodeReport err=1 nodeIdentify:%s (follower cannot mint node_id, retry leader)", strNodeIdentify.c_str());
        return 1u;
    }
    LOG4_ERROR("NodeReport err=1 nodeIdentify:%s", strNodeIdentify.c_str());
    return 1u;
}
} // namespace

bool CmdNodeReport::Init()
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
    return (true);
}

bool CmdNodeReport::AnyMessage(const net::tagMsgShell &stMsgShell, const MsgHead &oMsgHead, const MsgBody &oMsgBody)
{
    LOG4_TRACE("%s() NodeReport oMsgHead.cmd:%u)", __FUNCTION__, oMsgHead.cmd());
    NodeReport oNodeReport;
    NodeReportRsp oNodeReportRsp;
    uint32_t err = 1;
    bool boParsed = false;
    std::string strNodeIdentify;
    std::string strReporterNodeType;
    SessionRaftCluster *raft = GetSessionRaftCluster();
    if (oNodeReport.ParseFromString(oMsgBody.body()))
    {
        boParsed = true;
        strNodeIdentify = oNodeReport.node_ip() + std::string(":") + std::to_string(oNodeReport.node_port());
        strReporterNodeType = oNodeReport.node_type();
        LOG4_TRACE("%s() NodeReport oMsgHead.cmd:%u nodeIdentify:%s", __FUNCTION__, oMsgHead.cmd(), strNodeIdentify.c_str());
        uint16 unNodeId = m_pSessionOnlineNodes->AddNode(oNodeReport);
        err = ApplyNodeReportRaftOutcome(raft, m_pSessionOnlineNodes, unNodeId, strNodeIdentify, oNodeReport, oNodeReportRsp);
        if (unNodeId != 0)
        {
            LOG4_INFO("AddNode node_id(%u)!", unNodeId);
        }
    }
    else
    {
        LOG4_ERROR("failed to parse node info json from MsgBody.data()!");
    }
    oNodeReportRsp.set_errcode(err);
    if (raft)
    {
        raft->FillNodeReportRspRaftForResponse(oNodeReportRsp, err);
    }
    if (boParsed)
    {
        m_pSessionOnlineNodes->MaybeAttachSubscribedRouteSnapshotToRsp(strNodeIdentify, strReporterNodeType, false, err, oNodeReportRsp);
    }
    return (GetLabor()->SendToClient(stMsgShell, oMsgHead, oNodeReportRsp.SerializeAsString()));
}

} /* namespace coor */
