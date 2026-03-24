/*******************************************************************************
 * Project:  Center
 * @file     SessionOnlineNodes.hpp
 * @brief
 * @author   cjy
 * @date:     11.22, 2019
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_SESSIONNODESHOLDER_HPP_
#define SRC_SESSIONNODESHOLDER_HPP_
#include "google/protobuf/util/json_util.h"
#include "protocol/oss_sys.pb.h"
#include "SessionRaftCluster.hpp"

namespace coor
{

/**
 * @brief report node status（Raft 选主与 Center 间同步见 SessionRaftCluster）
 */
class SessionOnlineNodes : public net::Session
{
public:
    SessionOnlineNodes(const std::string &strSessionId, ev_tstamp dSessionTimeout, const std::string &strSessionClass)
        : net::Session(strSessionId, dSessionTimeout, strSessionClass)
    {
    }
    virtual ~SessionOnlineNodes() = default;
    static const std::string SessionClass() { return std::string("coor::SessionOnlineNodes"); }
    virtual net::E_CMD_STATUS Timeout();
    virtual bool Init(const util::CJsonObject &conf) override; //"CenterCmd.json"
    bool InitFromLocal(util::CJsonObject conf);
public:
    void AddIpwhite(const std::string &strIpwhite);
    void AddSubscribe(const std::string &strNodeType, const std::string &strBeSubscribeNodeType);

    uint16 AddNode(const NodeReport &oNodeReport, bool boRegister = false);
    /** need_leadership 时仅 Leader 可摘节点；Follower 无操作（避免本地断连误删副本） */
    void RemoveNode(const std::string &strNodeIdentify);

    void GetIpWhite(util::CJsonObject &oIpWhite) const;
    void GetCenter(util::CJsonObject &oCenter) const;
    void GetSubscription(util::CJsonObject &oSubcription) const;
    void GetSubscription(const std::string &strNodeType, util::CJsonObject &oSubcription) const;
    void GetOnlineNode(util::CJsonObject &oOnlineNode) const;
    void GetOnlineNode(const std::string &strNodeType, util::CJsonObject &oOnlineNode) const;
    bool GetNodeReport(const std::string &strNodeType, util::CJsonObject &oNodeReport) const;
    bool GetNodeReport(const std::string &strNodeType, const std::string &strIdentify, util::CJsonObject &oNodeReport) const;

    bool GetOnlineNode(const std::string &strNodeType, std::vector<std::string> &vecNodes);

    /** need_leadership 时等价于 SessionRaftCluster::IsRaftLeader() */
    bool IsLeadership() const;
    void BeLeader();
    void RelievedLeader();

    /** Raft 当选 Leader 后调用：把本机已有在线节点重新广播给订阅方（缓解换主后下游迟迟收不到路由） */
    void ReplaySubscriptionsAfterRaftLeadership();

    /** 每轮 Leader 向各 Follower 发 AE 前调用一次，递增快照版本号 */
    void BumpOnlineSnapshotSeqForRaft();
    /** 在 Bump 之后向本条 AppendEntries 写入当前 seq 与全量 online_nodes */
    void FillLeaderOnlineSnapshotForRaftAppend(RaftAppendEntries *ae);
    /** Follower 收到合法 AppendEntries 且 online_nodes_seq!=0 时整体替换本地副本 */
    void ApplyOnlineSnapshotFromLeader(const RaftAppendEntries &req);

    void AddSendingNodeNotice(const std::string &strToNodeIdentify, const NodeNotice &oNodeNotice)
    {
        m_mapSendingNodeNotice[strToNodeIdentify].push_back(std::make_pair(oNodeNotice, GetLabor()->GetNowTime()));
    }

    void RemoveSendingNodeNotice(const std::string &strToNodeIdentify)
    {
        auto iter = m_mapSendingNodeNotice.find(strToNodeIdentify);
        if (iter != m_mapSendingNodeNotice.end())
        {
            iter->second.pop_front();
        }
    }
protected:
    void CheckSendingNodeNotice();
    uint32 m_uiLastCheckSendingNodeNotice = GetLabor()->GetNowTime();

    void SendNodeNotice(const std::string &strToNodeIdentify, const NodeNotice &oNodeNotice, bool boPushSendingList = true);
    void AddNodeBroadcast(const NodeReport &oNodeReport, bool boRegister);
    void RemoveNodeBroadcast(const NodeReport &oNodeReport);

    uint32 m_uiServerTime = GetLabor()->GetNowTime();

    void CheckNodesBeat();
    uint32 m_uiNodeOverdue = 35; //节点心跳10s，超时35s
    uint32 m_uiLastCheckNodesBeat = 0;

private:
    bool m_bInit = false;

    bool m_boNeedLeadership = true;

    std::unordered_set<std::string> m_setIpwhite;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_mapPublisher; ///< map<node_type, set<subscribers_node_type> >
    std::unordered_map<std::string, std::string> m_mapIdentifyNodeId;                 ///< map<Identify, node_type>
    std::unordered_map<std::string, std::unordered_map<std::string, NodeReport>> m_mapOnlineNodes; ///< map<node_type, map<node_identify, NodeReport> >

    std::map<std::string, std::list<std::pair<NodeNotice, uint32>>> m_mapSendingNodeNotice; // map<to_node_Identify,list<NodeNotice,timestamp>>

    /** Leader 上：按节点限频，用心跳(NodeReport)补发路由，避免订阅方晚于首次广播上线时长时间无路由 */
    std::unordered_map<std::string, uint32> m_mapLastHeartbeatRouteBroadcast;

    /** 随 AppendEntries 下发的在线表快照版本号（单调递增，便于日志与排障） */
    uint64_t m_raftOnlineSnapshotSeq = 0;
};

inline SessionOnlineNodes *GetSessionOnlineNodes() { return net::GetGlobalConfigSession<SessionOnlineNodes>("CenterCmd.json", 1); }

} // namespace coor

#endif /* SRC_SESSIONNODESHOLDER_HPP_ */
