/*******************************************************************************
 * Project:  Center
 * @file     SessionOnlineNodes.cpp
 * @brief
 * @author   cjy
 * @date:    Sep 20, 2016
 * @note
 * Modify history:
 ******************************************************************************/
#include "SessionOnlineNodes.hpp"

namespace coor
{
namespace
{
/** NODE_REG_NOTICE 发送失败时的重试：原 10s 扫描 + 10s 才重发，易导致 Interface 8～10s 后才收到路由 */
constexpr uint32_t kNodeNoticeScanIntervalSec = 1u;
constexpr uint32_t kNodeNoticeRetryAfterSec = 2u;
constexpr uint32_t kNodeNoticeDropAfterSec = 30u;
/** Leader 上同节点两次「心跳补发路由」最小间隔（秒），与 Logic 上报周期同量级即可 */
constexpr uint32_t kMinHeartbeatRouteBroadcastSec = 8u;
} // namespace

bool SessionOnlineNodes::Init(const util::CJsonObject &conf)
{
    if (!m_bInit)
    {
        m_bInit = true;
        (void)GetSessionRaftCluster();
        return (InitFromLocal(conf));
    }
    return true;
}

bool SessionOnlineNodes::InitFromLocal(util::CJsonObject oCustomConf)
{
    oCustomConf.Get("need_leadership", m_boNeedLeadership);
    oCustomConf.Get("node_overdue", m_uiNodeOverdue);
    LOG4_TRACE("%s() boNeedLeadership(%d) uiNodeOverdue(%u)", __FUNCTION__, m_boNeedLeadership, m_uiNodeOverdue);
    for (int i = 0; i < oCustomConf["ipwhite"].GetArraySize(); ++i)
    {
        AddIpwhite(oCustomConf["ipwhite"](i));
    }
    for (int i = 0; i < oCustomConf["node_types"].GetArraySize(); ++i)
    {
        for (int j = 0; j < oCustomConf["node_types"][i]["subscribe"].GetArraySize(); ++j)
        {
            AddSubscribe(oCustomConf["node_types"][i]("node_type"), oCustomConf["node_types"][i]["subscribe"](j));
        }
    }
    return true;
}

net::E_CMD_STATUS SessionOnlineNodes::Timeout()
{
    LOG4_INFO("%s() WorkerIdentify(%s) IsLeader(%d)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), IsLeadership() ? 1 : 0);
    m_uiServerTime = GetLabor()->GetNowTime();
    CheckNodesBeat();
    CheckSendingNodeNotice();
    return (net::STATUS_CMD_RUNNING);
}

void SessionOnlineNodes::AddIpwhite(const std::string &strIpwhite)
{
    m_setIpwhite.insert(strIpwhite);
}

void SessionOnlineNodes::AddSubscribe(const std::string &strNodeType, const std::string &strBeSubscribeNodeType)
{
    auto pub_iter = m_mapPublisher.find(strBeSubscribeNodeType);
    if (pub_iter == m_mapPublisher.end())
    {
        std::unordered_set<std::string> setSubscriber;
        setSubscriber.insert(strNodeType);
        LOG4_TRACE("%s() AddSubscribe beSubscribeNodeType %s Subscriber(%s)", __FUNCTION__, strBeSubscribeNodeType.c_str(), strNodeType.c_str());
        m_mapPublisher.insert(std::make_pair(strBeSubscribeNodeType, setSubscriber));
    }
    else
    {
        pub_iter->second.insert(strNodeType);
    }
}

uint16 SessionOnlineNodes::AddNode(const NodeReport &oNodeReport, bool boRegister)
{
    SessionRaftCluster *raft = GetSessionRaftCluster();
    const bool raft_leader = raft && raft->IsRaftLeader();

    LOG4_TRACE("%s() AddNode oNodeReport WorkerIdentify(%s) oNodeInfo(%s)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(),
               oNodeReport.DebugString().c_str());
    std::string strNodeIdentify = oNodeReport.node_ip() + std::string(":") + std::to_string(oNodeReport.node_port());
    LOG4_TRACE("%s() AddNode oNodeReport strNodeIdentify(%s) boRegister(%d)", __FUNCTION__, strNodeIdentify.c_str(), boRegister);
    NodeReport oNodeInfoObj = oNodeReport;
    oNodeInfoObj.clear_node();
    oNodeInfoObj.clear_workers();

    const bool existing = (m_mapIdentifyNodeId.find(strNodeIdentify) != m_mapIdentifyNodeId.end());

    if (m_boNeedLeadership && !raft_leader)
    {
        if (!existing)
        {
            return 0;
        }
        auto node_type_iter = m_mapOnlineNodes.find(oNodeInfoObj.node_type());
        if (node_type_iter == m_mapOnlineNodes.end())
        {
            return 0;
        }
        auto node_iter = node_type_iter->second.find(strNodeIdentify);
        if (node_iter == node_type_iter->second.end())
        {
            return 0;
        }
        uint16 nid = oNodeInfoObj.node_id();
        if (nid == 0)
        {
            nid = node_iter->second.node_id();
        }
        oNodeInfoObj.set_node_id(nid);
        node_iter->second = oNodeInfoObj;
        LOG4_TRACE("%s() follower heartbeat update strNodeIdentify(%s) node_id(%u)", __FUNCTION__, strNodeIdentify.c_str(), (unsigned)nid);
        return nid;
    }

    if (oNodeInfoObj.node_id() == 0)
    {
        oNodeInfoObj.set_node_id(raft ? raft->AllocNextNodeId() : static_cast<uint16>(1));
        LOG4_INFO("%s() first register: assign node_id=%u strNodeIdentify(%s) node_type(%s)", __FUNCTION__, oNodeInfoObj.node_id(), strNodeIdentify.c_str(),
                  oNodeInfoObj.node_type().c_str());
    }

    auto identify_node_iter = m_mapIdentifyNodeId.find(strNodeIdentify);
    if (identify_node_iter == m_mapIdentifyNodeId.end())
    {
        m_mapIdentifyNodeId.insert(std::make_pair(strNodeIdentify, oNodeInfoObj.node_type()));
    }

    auto node_type_iter = m_mapOnlineNodes.find(oNodeInfoObj.node_type());
    if (node_type_iter == m_mapOnlineNodes.end())
    {
        std::unordered_map<std::string, NodeReport> mapNodeInfo;
        mapNodeInfo.insert(std::make_pair(strNodeIdentify, oNodeInfoObj));
        m_mapOnlineNodes.insert(std::make_pair(oNodeInfoObj.node_type(), mapNodeInfo));
        LOG4_TRACE("%s() try to broadcast for strNodeIdentify(%s)", __FUNCTION__, strNodeIdentify.c_str());
        AddNodeBroadcast(oNodeInfoObj, boRegister);
        return (oNodeInfoObj.node_id());
    }
    else
    {
        auto node_iter = node_type_iter->second.find(strNodeIdentify);
        if (node_iter == node_type_iter->second.end())
        {
            node_type_iter->second.insert(std::make_pair(strNodeIdentify, oNodeInfoObj));
            LOG4_TRACE("%s() try to broadcast for strNodeIdentify(%s)", __FUNCTION__, strNodeIdentify.c_str());
            AddNodeBroadcast(oNodeInfoObj, boRegister);
            return (oNodeInfoObj.node_id());
        }
        else
        {
            node_iter->second = oNodeInfoObj;
            LOG4_TRACE("%s() try to broadcast for strNodeIdentify(%s) boRegister(%d)", __FUNCTION__, strNodeIdentify.c_str(), boRegister);
            if (boRegister)
            {
                AddNodeBroadcast(oNodeInfoObj, boRegister);
            }
            else if (IsLeadership())
            {
                const uint32_t now = GetLabor()->GetNowTime();
                uint32_t &last = m_mapLastHeartbeatRouteBroadcast[strNodeIdentify];
                if (last == 0u || now >= last + kMinHeartbeatRouteBroadcastSec)
                {
                    last = now;
                    LOG4_TRACE("%s() heartbeat re-broadcast route for %s", __FUNCTION__, strNodeIdentify.c_str());
                    AddNodeBroadcast(oNodeInfoObj, false);
                }
            }
            return (oNodeInfoObj.node_id());
        }
    }
}

void SessionOnlineNodes::RemoveNode(const std::string &strNodeIdentify)
{
    // need_leadership 时在线表以 Leader 为准；Follower 上 TCP 断连不代表业务下线（见 Center-Raft-ASCII-Flow 从节点路由视图）
    if (m_boNeedLeadership && !IsLeadership())
    {
        LOG4_TRACE("%s() skip remove on non-leader nodeIdentify(%s)", __FUNCTION__, strNodeIdentify.c_str());
        return;
    }
    LOG4_TRACE("%s() GetWorkerIdentify(%s) oNodeInfo(%s)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), strNodeIdentify.c_str());
    auto identity_node_iter = m_mapIdentifyNodeId.find(strNodeIdentify);
    if (identity_node_iter != m_mapIdentifyNodeId.end())
    {
        auto node_type_iter = m_mapOnlineNodes.find(identity_node_iter->second);
        if (node_type_iter != m_mapOnlineNodes.end())
        {
            auto node_iter = node_type_iter->second.find(strNodeIdentify);
            if (node_iter != node_type_iter->second.end())
            {
                if (IsLeadership())
                {
                    RemoveNodeBroadcast(node_iter->second);
                }
                node_type_iter->second.erase(node_iter);
            }
        }
        m_mapIdentifyNodeId.erase(strNodeIdentify);
    }
    m_mapLastHeartbeatRouteBroadcast.erase(strNodeIdentify);
}

void SessionOnlineNodes::GetIpWhite(util::CJsonObject &oIpWhite) const
{
    for (auto it : m_setIpwhite)
    {
        oIpWhite.Add(it);
    }
}

void SessionOnlineNodes::GetCenter(util::CJsonObject &oCenter) const
{
    SessionRaftCluster *raft = GetSessionRaftCluster();
    if (raft)
    {
        raft->GetRaftCenterJson(oCenter);
    }
}

void SessionOnlineNodes::GetSubscription(util::CJsonObject &oSubcription) const
{
    for (const auto &pub_iter : m_mapPublisher)
    {
        oSubcription.AddAsFirst(util::CJsonObject("{}"));
        oSubcription[0].Add("node_type", pub_iter.first);
        oSubcription[0].AddEmptySubArray("subcriber");
        for (const auto &it : pub_iter.second)
        {
            oSubcription[0]["subcriber"].Add(it);
        }
    }
}

void SessionOnlineNodes::GetSubscription(const std::string &strNodeType, util::CJsonObject &oSubcription) const
{
    auto pub_iter = m_mapPublisher.find(strNodeType);
    if (pub_iter != m_mapPublisher.end())
    {
        for (auto it = pub_iter->second.begin(); it != pub_iter->second.end(); ++it)
        {
            oSubcription.Add(*it);
        }
    }
}

void SessionOnlineNodes::GetOnlineNode(util::CJsonObject &oOnlineNode) const
{
    for (const auto &node_iter : m_mapOnlineNodes)
    {
        oOnlineNode.AddAsFirst(util::CJsonObject("{}"));
        oOnlineNode[0].Add("node_type", node_iter.first);
        oOnlineNode[0].AddEmptySubArray("node");
        for (const auto &it : node_iter.second)
        {
            oOnlineNode[0]["node"].Add(it.second.node_ip() + ":" + std::to_string(it.second.node_port()));
        }
    }
}

void SessionOnlineNodes::GetOnlineNode(const std::string &strNodeType, util::CJsonObject &oOnlineNode) const
{
    auto node_iter = m_mapOnlineNodes.find(strNodeType);
    if (node_iter != m_mapOnlineNodes.end())
    {
        for (const auto &it : node_iter->second)
        {
            oOnlineNode.Add(it.second.node_ip() + ":" + std::to_string(it.second.node_port()));
        }
    }
}

bool SessionOnlineNodes::GetNodeReport(const std::string &strNodeType, util::CJsonObject &oNodeReport) const
{
    LOG4_TRACE("%s() strNodeType:%s", __FUNCTION__, strNodeType.c_str());
    auto node_iter = m_mapOnlineNodes.find(strNodeType);
    if (node_iter == m_mapOnlineNodes.end())
    {
        return (false);
    }
    else
    {
        util::CJsonObject oJson;
        for (const auto &it : node_iter->second)
        {
            if (net::Pb2Json(it.second, oJson))
            {
                oJson.Delete("worker");
                oNodeReport.AddAsFirst(oJson);
            }
        }
        return (true);
    }
}

bool SessionOnlineNodes::GetNodeReport(const std::string &strNodeType, const std::string &strIdentify, util::CJsonObject &oNodeReport) const
{
    LOG4_TRACE("%s() strNodeType:%s strIdentify:%s", __FUNCTION__, strNodeType.c_str(), strIdentify.c_str());
    auto node_iter = m_mapOnlineNodes.find(strNodeType);
    if (node_iter == m_mapOnlineNodes.end())
    {
        return (false);
    }
    else
    {
        auto it = node_iter->second.find(strIdentify);
        if (it == node_iter->second.end())
        {
            return (false);
        }
        else
        {
            util::CJsonObject oJson;
            if (net::Pb2Json(it->second, oJson))
            {
                oNodeReport.Add(oJson);
            }
            LOG4_TRACE("%s() oNodeReport:%s", __FUNCTION__, oNodeReport.ToString().c_str());
            return (true);
        }
    }
}

bool SessionOnlineNodes::GetOnlineNode(const std::string &strNodeType, std::vector<std::string> &vecNodes)
{
    auto node_iter = m_mapOnlineNodes.find(strNodeType);
    if (node_iter != m_mapOnlineNodes.end())
    {
        for (const auto &it : node_iter->second)
        {
            vecNodes.push_back(it.second.node_ip() + ":" + std::to_string(it.second.node_port()));
        }
        return (true);
    }
    return (false);
}

bool SessionOnlineNodes::IsLeadership() const
{
    if (m_boNeedLeadership)
    {
        SessionRaftCluster *raft = GetSessionRaftCluster();
        return raft && raft->IsRaftLeader();
    }
    return true;
}

void SessionOnlineNodes::BeLeader()
{
    SessionRaftCluster *raft = GetSessionRaftCluster();
    if (raft)
    {
        raft->BeLeader();
    }
}

void SessionOnlineNodes::RelievedLeader()
{
    SessionRaftCluster *raft = GetSessionRaftCluster();
    if (raft)
    {
        raft->RelievedLeader();
    }
}

void SessionOnlineNodes::ReplaySubscriptionsAfterRaftLeadership()
{
    if (!IsLeadership())
    {
        return;
    }
    LOG4_INFO("%s() rebroadcast all online nodes to subscribers (raft leader)", __FUNCTION__);
    for (const auto &byType : m_mapOnlineNodes)
    {
        for (const auto &byIdent : byType.second)
        {
            AddNodeBroadcast(byIdent.second, true);
        }
    }
}

void SessionOnlineNodes::BumpOnlineSnapshotSeqForRaft()
{
    if (!m_boNeedLeadership || !IsLeadership())
    {
        return;
    }
    ++m_raftOnlineSnapshotSeq;
}

void SessionOnlineNodes::FillLeaderOnlineSnapshotForRaftAppend(RaftAppendEntries *ae)
{
    if (ae == nullptr || !m_boNeedLeadership || !IsLeadership() || m_raftOnlineSnapshotSeq == 0)
    {
        return;
    }
    ae->set_online_nodes_seq(m_raftOnlineSnapshotSeq);
    ae->clear_online_nodes();
    for (const auto &byType : m_mapOnlineNodes)
    {
        for (const auto &byIdent : byType.second)
        {
            const NodeReport &nr = byIdent.second;
            RaftOnlineNodeEntry *e = ae->add_online_nodes();
            e->set_node_type(nr.node_type());
            e->set_node_id(nr.node_id());
            e->set_node_ip(nr.node_ip());
            e->set_node_port(nr.node_port());
            e->set_access_ip(nr.access_ip());
            e->set_access_port(nr.access_port());
            e->set_worker_num(nr.worker_num());
            e->set_active_time(nr.active_time());
        }
    }
    LOG4_TRACE("%s() seq=%llu entries=%d", __FUNCTION__, (unsigned long long)m_raftOnlineSnapshotSeq, ae->online_nodes_size());
}

void SessionOnlineNodes::ApplyOnlineSnapshotFromLeader(const RaftAppendEntries &req)
{
    if (req.online_nodes_seq() == 0u)
    {
        return;
    }
    if (!m_boNeedLeadership || IsLeadership())
    {
        return;
    }
    m_mapOnlineNodes.clear();
    m_mapIdentifyNodeId.clear();
    m_mapLastHeartbeatRouteBroadcast.clear();
    for (int i = 0; i < req.online_nodes_size(); ++i)
    {
        const RaftOnlineNodeEntry &e = req.online_nodes(i);
        NodeReport nr;
        nr.set_node_type(e.node_type());
        nr.set_node_id(e.node_id());
        nr.set_node_ip(e.node_ip());
        nr.set_node_port(e.node_port());
        nr.set_access_ip(e.access_ip());
        nr.set_access_port(e.access_port());
        nr.set_worker_num(e.worker_num());
        nr.set_active_time(e.active_time());
        nr.clear_node();
        nr.clear_workers();
        const std::string id = nr.node_ip() + std::string(":") + std::to_string(nr.node_port());
        m_mapIdentifyNodeId[id] = nr.node_type();
        m_mapOnlineNodes[nr.node_type()][id] = std::move(nr);
    }
    LOG4_TRACE("%s() seq=%llu entries=%d", __FUNCTION__, (unsigned long long)req.online_nodes_seq(), req.online_nodes_size());
}

void SessionOnlineNodes::CheckSendingNodeNotice()
{
    m_uiServerTime = GetLabor()->GetNowTime();
    if (m_uiServerTime >= m_uiLastCheckSendingNodeNotice + kNodeNoticeScanIntervalSec)
    {
        m_uiLastCheckSendingNodeNotice = m_uiServerTime;
        for (auto &iter : m_mapSendingNodeNotice)
        {
            auto &sendingList = iter.second;
            while (sendingList.size() > 0)
            {
                if (sendingList.front().second + kNodeNoticeRetryAfterSec <= m_uiServerTime)
                {
                    LOG4_TRACE("%s() sending to %s.timestamp(%u) uiServerTime(%u)  CMD_REQ_NODE_REG_NOTICE:%s!", __FUNCTION__, iter.first.c_str(),
                               sendingList.front().second, m_uiServerTime, sendingList.front().first.DebugString().c_str());
                    SendNodeNotice(iter.first, sendingList.front().first, false);
                    break;
                }
                else if (sendingList.front().second + kNodeNoticeDropAfterSec <= m_uiServerTime)
                {
                    LOG4_ERROR("%s() sending to %s failed.timestamp(%u) uiServerTime(%u)  CMD_REQ_NODE_REG_NOTICE:%s!", __FUNCTION__, iter.first.c_str(),
                               sendingList.front().second, m_uiServerTime, sendingList.front().first.DebugString().c_str());
                    sendingList.pop_front();
                }
                else
                {
                    break;
                }
            }
        }
    }
}

void SessionOnlineNodes::SendNodeNotice(const std::string &strToNodeIdentify, const NodeNotice &oNodeNotice, bool boPushSendingList)
{
    auto callback = [](const MsgHead &oInMsgHead, const MsgBody &oInMsgBody, net::StepParam *data, net::Session *pSession)
    {
        (void)oInMsgHead;
        (void)oInMsgBody;
        SessionOnlineNodes *pSessionOnlineNodes = (SessionOnlineNodes *)pSession;
        DataStepCustom *pdata = (DataStepCustom *)data;
        if (pSessionOnlineNodes && pdata && pdata->strToNodeIdentify.size() > 0)
        {
            pSessionOnlineNodes->RemoveSendingNodeNotice(pdata->strToNodeIdentify);
            LOG4_TRACE("NODE_REG_NOTICE response:send to %s succ for msg body:%s!", pdata->strToNodeIdentify.c_str(), oInMsgBody.body().c_str());
        }
        else
        {
            LOG4_WARN("NODE_REG_NOTICE response:failed to send to %s for msg body:%s!", pdata->strToNodeIdentify.c_str(), oInMsgBody.body().c_str());
        }
    };
    LOG4_TRACE("%s() NODE_REG_NOTICE request:sending to %s oNodeNotice:%s!", __FUNCTION__, strToNodeIdentify.c_str(), oNodeNotice.DebugString().c_str());
    GetLabor()->SendToCallback(this, net::CMD_REQ_NODE_REG_NOTICE, oNodeNotice.SerializeAsString(), callback, strToNodeIdentify, "", new DataStepCustom(strToNodeIdentify));
    if (boPushSendingList)
    {
        AddSendingNodeNotice(strToNodeIdentify, oNodeNotice);
    }
}

void SessionOnlineNodes::AddNodeBroadcast(const NodeReport &oNodeReport, bool boRegister)
{
    (void)boRegister;
    const bool boLeadership = IsLeadership();
    LOG4_TRACE("%s() WorkerIdentify(%s) IsLeadership(%d)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), boLeadership);
    if (!boLeadership)
    {
        return;
    }
    LOG4_TRACE("%s() %s", __FUNCTION__, oNodeReport.DebugString().c_str());
    NodeNotice oSubcribeNodeInfo;
    NodeNotice oAddNodes;
    NodeReport oAddedNodeInfo = oNodeReport;
    oAddedNodeInfo.clear_node();
    oAddedNodeInfo.clear_workers();
    const std::string &reportNodeType = oNodeReport.node_type();
    (*oAddNodes.add_node_arry_reg()) = std::move(oAddedNodeInfo);
    LOG4_TRACE("%s() mapPublisher.size %zu ", __FUNCTION__, m_mapPublisher.size());
    for (const auto &publisher_iter : m_mapPublisher)
    {
        const std::string &subscribedNodeType = publisher_iter.first;
        for (const auto &subscriberNodeType : publisher_iter.second)
        {
            if (subscribedNodeType == oNodeReport.node_type())
            {
                auto onlineNodesIter = m_mapOnlineNodes.find(subscriberNodeType);
                if (onlineNodesIter != m_mapOnlineNodes.end())
                {
                    LOG4_TRACE("mapOnlineNodes[%s].size() = %zu", subscriberNodeType.c_str(), onlineNodesIter->second.size());
                    for (const auto &node_iter : onlineNodesIter->second)
                    {
                        LOG4_INFO("send this report node info to subscriber node identity(%s).report node info:%s", node_iter.first.c_str(),
                                  oAddNodes.DebugString().c_str());
                        SendNodeNotice(node_iter.first, oAddNodes);
                    }
                }
            }
            LOG4_TRACE("%s() mapOnlineNodes's size %zu reportNodeType(%s) subscribedNodeType(%s) subscriberNodeType(%s)", __FUNCTION__, m_mapOnlineNodes.size(),
                       reportNodeType.c_str(), subscribedNodeType.c_str(), subscriberNodeType.c_str());
            if ((subscriberNodeType) == oNodeReport.node_type())
            {
                LOG4_TRACE("%s() subscribedNodeType(%s) subscriberNodeType(%s)", __FUNCTION__, subscribedNodeType.c_str(), subscriberNodeType.c_str());
                auto onlineNodesIter = m_mapOnlineNodes.find(subscribedNodeType);
                if (onlineNodesIter != m_mapOnlineNodes.end())
                {
                    LOG4_TRACE("%s() onlineNodes subscribedNodeType(%s) subscriberNodeType(%s)", __FUNCTION__, subscribedNodeType.c_str(),
                               subscriberNodeType.c_str());
                    for (const auto &node_iter : onlineNodesIter->second)
                    {
                        NodeReport oExistNodeInfo = node_iter.second;
                        LOG4_TRACE("oExistNodeInfo(%s)", oExistNodeInfo.DebugString().c_str());
                        oExistNodeInfo.clear_node();
                        oExistNodeInfo.clear_workers();
                        (*oSubcribeNodeInfo.add_node_arry_reg()) = std::move(oExistNodeInfo);
                    }
                }
            }
        }
    }

    if (oSubcribeNodeInfo.node_arry_reg_size() > 0)
    {
        char szThisNodeIdentity[32];
        snprintf(szThisNodeIdentity, sizeof(szThisNodeIdentity), "%s:%u", oNodeReport.node_ip().c_str(), oNodeReport.node_port());
        LOG4_INFO("send subscribe node info to the report node identity(%s)", szThisNodeIdentity);
        SendNodeNotice(szThisNodeIdentity, oSubcribeNodeInfo);
    }
    util::CJsonObject oOnlineNode;
    GetOnlineNode(oOnlineNode);
    LOG4_TRACE("%s() AddNodeBroadcast oOnlineNode(%s)", __FUNCTION__, oOnlineNode.ToString().c_str());
}

void SessionOnlineNodes::RemoveNodeBroadcast(const NodeReport &oNodeReport)
{
    bool boLeadership = IsLeadership();
    LOG4_TRACE("%s() IsLeadership(%d)", __FUNCTION__, boLeadership);
    if (!boLeadership)
    {
        return;
    }
    LOG4_TRACE("%s() %s", __FUNCTION__, oNodeReport.DebugString().c_str());
    NodeNotice oDelNodes;
    NodeReport oDeletedNodeInfo = oNodeReport;

    LOG4_TRACE("(%s)", oDeletedNodeInfo.DebugString().c_str());
    (*oDelNodes.add_node_arry_exit()) = std::move(oDeletedNodeInfo);
    LOG4_TRACE("(%s)", oDelNodes.DebugString().c_str());
    for (auto &sub_iter : m_mapPublisher)
    {
        for (auto &node_type_iter : sub_iter.second)
        {
            if (sub_iter.first == oNodeReport.node_type())
            {
                auto node_list_iter = m_mapOnlineNodes.find(node_type_iter);
                if (node_list_iter != m_mapOnlineNodes.end())
                {
                    for (auto node_iter = node_list_iter->second.begin(); node_iter != node_list_iter->second.end(); ++node_iter)
                    {
                        SendNodeNotice(node_iter->first, oDelNodes);
                    }
                }
            }
        }
    }
}

void SessionOnlineNodes::CheckNodesBeat()
{
    if (!IsLeadership())
    {
        return;
    }
    if (m_uiNodeOverdue > 0)
    {
        m_uiServerTime = GetLabor()->GetNowTime();
        if (m_uiServerTime >= m_uiLastCheckNodesBeat + 10)
        {
            m_uiLastCheckNodesBeat = m_uiServerTime;
            std::vector<std::string> removeNodeIdentifys;
            for (const auto &iterMapNode : m_mapOnlineNodes)
            {
                for (const auto &iterNodeIdentify : iterMapNode.second)
                {
                    if (m_uiServerTime > (iterNodeIdentify.second.active_time() + m_uiNodeOverdue))
                    {
                        LOG4_TRACE("%s() removeNodeIdentify iterNodeIdentify(%s,%lf) uiServerTime(%u) uiNodeOverdue(%u)", __FUNCTION__,
                                   iterNodeIdentify.first.c_str(), iterNodeIdentify.second.active_time(), m_uiServerTime, m_uiNodeOverdue);
                        removeNodeIdentifys.push_back(iterNodeIdentify.first);
                    }
                }
            }
            for (const auto &iterRemoveNodeIdentify : removeNodeIdentifys)
            {
                RemoveNode(iterRemoveNodeIdentify);
            }
        }
    }
}

} // namespace coor
