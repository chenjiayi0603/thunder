/*******************************************************************************
 * Project:  Center
 * @file     CmdRegister.cpp
 * @brief
 * @author   cjy
 * @date:    Sep 19, 2016
 * @note
 * Modify history:
 ******************************************************************************/
#include "protocol/oss_sys.pb.h"
#include "CmdNodeRegister.hpp"
#include "SessionRaftCluster.hpp"

MUDULE_CREATE(coor::CmdNodeRegister);

namespace coor
{
namespace
{
/**
 * @brief 注册结果中的 Raft 语义：err 与 NodeReportRsp 字段（与 SessionRaftCluster::FillNodeReportRspRaftForResponse 配套）
 */
uint32_t ApplyNodeRegisterRaftOutcome(SessionRaftCluster *raft, SessionOnlineNodes *nodes, uint16_t unNodeId, const std::string &strNodeIdentify,
                                      NodeReportRsp &rsp)
{
    if (unNodeId != 0)
    {
        rsp.set_node_id(unNodeId);
        return 0u;
    }
    rsp.clear_node_id();
    if (!raft || !raft->RaftHasStableLeader())
    {
        LOG4_WARN("NodeRegister err=2 nodeIdentify:%s (no stable raft leader)", strNodeIdentify.c_str());
        return 2u;
    }
    if (!nodes || !nodes->IsLeadership())
    {
        LOG4_WARN("NodeRegister err=1 nodeIdentify:%s (not raft leader, retry leader)", strNodeIdentify.c_str());
        return 1u;
    }
    LOG4_ERROR("failed to AddNode nodeIdentify:%s while local raft leader", strNodeIdentify.c_str());
    return 1u;
}
} // namespace

bool CmdNodeRegister::Init()
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

bool CmdNodeRegister::AnyMessage(const net::tagMsgShell &stMsgShell, const MsgHead &oMsgHead, const MsgBody &oMsgBody)
{
    LOG4_TRACE("%s() NodeRegister oMsgHead.cmd:%u)", __FUNCTION__, oMsgHead.cmd());
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
        LOG4_TRACE("%s() NodeRegister oMsgHead.cmd:%u nodeIdentify:%s", __FUNCTION__, oMsgHead.cmd(), strNodeIdentify.c_str());
        uint16 unNodeId = m_pSessionOnlineNodes->AddNode(oNodeReport, true);
        err = ApplyNodeRegisterRaftOutcome(raft, m_pSessionOnlineNodes, unNodeId, strNodeIdentify, oNodeReportRsp);
        if (unNodeId != 0)
        {
            LOG4_INFO("NodeRegister AddNode node_id(%u)!", unNodeId);
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
        m_pSessionOnlineNodes->MaybeAttachSubscribedRouteSnapshotToRsp(strNodeIdentify, strReporterNodeType, true, err, oNodeReportRsp);
    }
    LOG4_INFO("oNodeReportRsp(%s)!", oNodeReportRsp.DebugString().c_str());
    return GetLabor()->SendToClient(stMsgShell, oMsgHead, oNodeReportRsp.SerializeAsString());
}

} /* namespace coor */
