/*******************************************************************************
 * Project:  Center
 * @file     SessionRaftCluster.cpp
 * @brief    Center 间 Raft（独立 Session）
 *
 * 关键路径（与头文件声明对应）：
 *   InitElection -> Timeout/RaftTick -> RaftStartElection -> HandleRaftRequestVote / OnRaftVoteResponse
 *   -> RaftBecomeLeader -> Timeout -> RaftSendAppendEntriesToAll -> HandleRaftAppendEntries
 ******************************************************************************/
#include "SessionRaftCluster.hpp"
#include "SessionOnlineNodes.hpp"
#include <algorithm>
#include <cstdlib>

namespace coor
{
namespace
{
/** 略收紧以缩短多 Center 冷启动选主时间（仍保留随机退避避免活锁） */
constexpr ev_tstamp kFollowerElectionBase = 0.18;
constexpr ev_tstamp kFollowerElectionRand = 0.22;
constexpr ev_tstamp kCandidateRetryBase = 0.08;
constexpr ev_tstamp kCandidateRetryRand = 0.12;

void RaftVoteCallback(const MsgHead &oInMsgHead, const MsgBody &oInMsgBody, net::StepParam *data, net::Session *pSession)
{
    (void)oInMsgHead;
    auto *sn = dynamic_cast<SessionRaftCluster *>(pSession);
    if (!sn)
    {
        return;
    }
    RaftRequestVoteRsp rsp;
    if (rsp.ParseFromString(oInMsgBody.body()))
    {
        sn->OnRaftVoteResponse(data, rsp);
    }
}

void RaftAppendCallback(const MsgHead &oInMsgHead, const MsgBody &oInMsgBody, net::StepParam * /*data*/, net::Session *pSession)
{
    (void)oInMsgHead;
    auto *sn = dynamic_cast<SessionRaftCluster *>(pSession);
    if (!sn)
    {
        return;
    }
    RaftAppendEntriesRsp rsp;
    if (rsp.ParseFromString(oInMsgBody.body()))
    {
        sn->OnRaftAppendEntriesResponse(rsp);
    }
}
} // namespace

/*
 * Init 流程：
 *   [已应用?] --是--> return
 *        |
 *       否
 *        v
 *   读 center_beat；centers：优先 CenterCmd.json；缺或空数组则回退 Center.json 的 custom.centers
 */
bool SessionRaftCluster::Init(const util::CJsonObject &conf)
{
    if (m_configApplied)
    {
        return true;
    }
    m_configApplied = true;
    conf.Get("center_beat", m_uiCenterBeat);
    util::CJsonObject centers;
    bool hasCenters = false;
    if (conf.Get("centers", centers) && centers.GetArraySize() > 0)
    {
        hasCenters = true;
    }
    else
    {
        centers.Clear();
        if (GetLabor()->GetCustomConf().Get("centers", centers) && centers.GetArraySize() > 0)
        {
            hasCenters = true;
        }
    }
    if (!hasCenters)
    {
        centers.Clear();
    }
    InitElection(centers);
    return true;
}

/*
 * Timeout 流程（每轮会话定时器）：
 *   更新 now -> RaftTick（Follower/Candidate 可能进入选举）
 *   若 server 时间跨过 center_beat 且本机是 Leader -> RaftSendAppendEntriesToAll
 */
net::E_CMD_STATUS SessionRaftCluster::Timeout()
{
    m_uiServerTime = GetLabor()->GetNowTime();
    const ev_tstamp now = GetLabor()->GetTimeStamp();
    RaftTick(now);

    if (m_uiServerTime >= m_uiLastSendCenterBeat + m_uiCenterBeat)
    {
        if (m_bIsLeader)
        {
            LOG4_INFO("%s() WorkerIdentify(%s) IsRaftLeader(%d)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), m_bIsLeader ? 1 : 0);
            RaftSendAppendEntriesToAll();
        }
        m_uiLastSendCenterBeat = m_uiServerTime;
    }
    return net::STATUS_CMD_RUNNING;
}

/*
 * InitElection：根据 centers 数组构造 peer 列表与多数派阈值。
 *
 *   n==0  --> 仅 self 入表 -> 单节点：直接 term=1、Leader
 *   n>0   --> 每项 + ".0" 为 identify；若列表未含本机 identify 则补入本机（避免 majority 退化为 1、多 Center 各自误选主）
 *           -> 排序；remote = 除 self 外全部
 *           -> 多节点：Follower，随机 follower_deadline 后才有机会 RaftStartElection
 */
void SessionRaftCluster::InitElection(const util::CJsonObject &oCenter)
{
    const std::string self = GetLabor()->GetWorkerIdentify();
    m_raftClusterPeers.clear();
    m_raftRemotePeers.clear();
    util::CJsonObject oCenterList = oCenter;
    const int n = oCenterList.GetArraySize();
    if (n == 0)
    {
        m_raftClusterPeers.push_back(self);
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            m_raftClusterPeers.push_back(oCenterList(i) + ".0");
        }
    }
    if (std::find(m_raftClusterPeers.begin(), m_raftClusterPeers.end(), self) == m_raftClusterPeers.end())
    {
        m_raftClusterPeers.push_back(self);
    }
    std::sort(m_raftClusterPeers.begin(), m_raftClusterPeers.end());
    for (const auto &id : m_raftClusterPeers)
    {
        if (id != self)
        {
            m_raftRemotePeers.push_back(id);
        }
    }
    m_raftMajority = m_raftClusterPeers.empty() ? 1u : (m_raftClusterPeers.size() / 2 + 1);
    m_raftSingleNode = (m_raftClusterPeers.size() == 1u && m_raftClusterPeers[0] == self);

    const ev_tstamp now = GetLabor()->GetTimeStamp();
    m_raftTerm = 0;
    m_raftVotedFor.clear();
    m_raftVotesGranted.clear();
    m_raftLeaderId.clear();
    m_raftLastLeaderContact = 0;
    m_raftLastAppendSend = 0;

    if (m_raftSingleNode)
    {
        m_raftTerm = 1;
        m_raftRole = CenterRaftRole::Leader;
        m_raftLeaderId = self;
        m_bIsLeader = true;
        m_uiBeLeaderTime = util::GetMicrosecond();
        m_raftLastLeaderContact = now;
        LOG4_TRACE("%s() single-node raft leader self(%s)", __FUNCTION__, self.c_str());
    }
    else
    {
        m_raftRole = CenterRaftRole::Follower;
        m_bIsLeader = false;
        m_uiBeLeaderTime = 0;
        m_raftFollowerDeadline = now + kFollowerElectionBase + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kFollowerElectionRand;
        m_raftCandidateDeadline = now;
        LOG4_TRACE("%s() multi-node raft follower, first election after %.3f", __FUNCTION__, m_raftFollowerDeadline - now);
    }
}

// 返回当前游标对应 id，并递增；到 NODE_ID_MAX 回绕到 1；禁止返回 0。
uint16_t SessionRaftCluster::AllocNextNodeId()
{
    const uint16 id = m_uiNextNodeIdAlloc;
    if (++m_uiNextNodeIdAlloc >= static_cast<uint16>(NODE_ID_MAX))
    {
        m_uiNextNodeIdAlloc = 1;
    }
    return (id == 0 ? static_cast<uint16>(1) : id);
}

// 按 m_raftClusterPeers 顺序输出；与 m_raftLeaderId 相等的项标记 leader=yes（admin show center）。
void SessionRaftCluster::GetRaftCenterJson(util::CJsonObject &oCenter) const
{
    for (const auto &ident : m_raftClusterPeers)
    {
        util::CJsonObject oNode;
        oNode.Add("identify", ident);
        if (!m_raftLeaderId.empty() && ident == m_raftLeaderId)
        {
            oNode.Add("leader", "yes");
        }
        else
        {
            oNode.Add("leader", "no");
        }
        oNode.Add("online", "yes");
        oCenter.Add(oNode);
    }
}

// 直接调用 RaftBecomeLeader(now)，不经过选举；一般仅兼容旧路径。
void SessionRaftCluster::BeLeader()
{
    RaftBecomeLeader(GetLabor()->GetTimeStamp());
}

// 若当前自认 Leader，则清标志与 be_leader 时间，角色改为 Follower（term/leader_id 不在这里清）。
void SessionRaftCluster::RelievedLeader()
{
    if (m_bIsLeader)
    {
        LOG4_TRACE("%s() WorkerIdentify(%s) RelievedLeader", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str());
        m_bIsLeader = false;
        m_uiBeLeaderTime = 0;
        m_raftRole = CenterRaftRole::Follower;
    }
}

/*
 * RaftBecomeFollower(term)：
 *   term > 本地 -> 更新 m_raftTerm
 *   角色 -> Follower；清空 votedFor；下台（m_bIsLeader=false）
 */
void SessionRaftCluster::RaftBecomeFollower(uint64_t term)
{
    const bool was_leader = m_bIsLeader;
    if (term > m_raftTerm)
    {
        m_raftTerm = term;
    }
    m_raftRole = CenterRaftRole::Follower;
    m_raftVotedFor.clear();
    m_bIsLeader = false;
    m_uiBeLeaderTime = 0;
    if (was_leader)
    {
        LOG4_INFO("%s() step down from leader term=%llu", __FUNCTION__, (unsigned long long)m_raftTerm);
    }
}

/*
 * RaftBecomeLeader：
 *   写 Leader 角色、m_raftLeaderId=本机、记录 be_leader 时间、清 votedFor、刷新 last_contact。
 */
void SessionRaftCluster::RaftBecomeLeader(ev_tstamp now)
{
    m_raftRole = CenterRaftRole::Leader;
    m_raftLeaderId = GetLabor()->GetWorkerIdentify();
    m_bIsLeader = true;
    m_uiBeLeaderTime = util::GetMicrosecond();
    m_raftVotedFor.clear();
    m_raftLastAppendSend = 0;
    m_raftLastLeaderContact = now;
    LOG4_INFO("%s() became raft leader term=%llu identify(%s)", __FUNCTION__, (unsigned long long)m_raftTerm, m_raftLeaderId.c_str());

    RaftSendAppendEntriesToAll();

    SessionOnlineNodes *online = GetSessionOnlineNodes();
    if (online)
    {
        online->ReplaySubscriptionsAfterRaftLeadership();
    }
}

// Candidate 或尚无 leader_id 视为未稳定，避免对外误导「当前 Leader」。
bool SessionRaftCluster::RaftHasStableLeader() const
{
    if (m_raftRole == CenterRaftRole::Candidate)
    {
        return false;
    }
    if (m_raftLeaderId.empty())
    {
        return false;
    }
    return true;
}

/*
 * FillNodeReportRspRaftForResponse：始终带 raft_term。
 *   errcode==2 -> 清 leader 字段
 *   errcode==0 -> 有稳定 Leader 则填 current_leader_identify，否则清空
 *   其它       -> 若有稳定 Leader 则填，否则清空
 */
void SessionRaftCluster::FillNodeReportRspRaftForResponse(NodeReportRsp &rsp, uint32_t errcode) const
{
    rsp.set_raft_term(m_raftTerm);
    if (errcode == 2u)
    {
        rsp.clear_current_leader_identify();
        return;
    }
    if (errcode == 0u)
    {
        if (RaftHasStableLeader())
        {
            rsp.set_current_leader_identify(m_raftLeaderId);
        }
        else
        {
            rsp.clear_current_leader_identify();
        }
        return;
    }
    if (RaftHasStableLeader())
    {
        rsp.set_current_leader_identify(m_raftLeaderId);
    }
    else
    {
        rsp.clear_current_leader_identify();
    }
}

/*
 * RaftTick（非 Leader、非单节点）：
 *
 *   Candidate && now >= candidate_deadline --> RaftStartElection（重试拉票）
 *   Follower:
 *     若曾收到 Leader（last_leader_contact>0）且未超时 -> 保持
 *     否则若未到首次 follower_deadline -> 保持
 *     否则 -> RaftStartElection
 */
void SessionRaftCluster::RaftTick(ev_tstamp now)
{
    if (m_raftSingleNode)
    {
        return;
    }
    if (m_raftRole == CenterRaftRole::Leader)
    {
        return;
    }
    if (m_raftRole == CenterRaftRole::Candidate)
    {
        if (now >= m_raftCandidateDeadline)
        {
            RaftStartElection(now);
        }
        return;
    }
    if (m_raftLastLeaderContact > 0.0)
    {
        const ev_tstamp follower_timeout = kFollowerElectionBase + kFollowerElectionRand;
        if (now < m_raftLastLeaderContact + follower_timeout)
        {
            return;
        }
    }
    else if (now < m_raftFollowerDeadline)
    {
        return;
    }
    RaftStartElection(now);
}

/*
 * RaftStartElection：
 *   role=Candidate；term++；votedFor=self；election_term=当前 term；票数集合含自己
 *   向每个 remote SendToCallback(RequestVote)，Step 带 DataStepCustom(peer) 以便回调里记账
 *   RequestVote 内携带 next_node_id_alloc_hint（供对端回包参考；游标以 Leader 上任后 AppendEntries 为准）
 */
void SessionRaftCluster::RaftStartElection(ev_tstamp now)
{
    if (m_raftSingleNode)
    {
        return;
    }
    m_raftRole = CenterRaftRole::Candidate;
    ++m_raftTerm;
    m_raftVotedFor = GetLabor()->GetWorkerIdentify();
    m_raftLeaderId.clear();
    m_bIsLeader = false;
    m_uiBeLeaderTime = 0;
    m_raftElectionTerm = m_raftTerm;
    m_raftVotesGranted.clear();
    m_raftVotesGranted.insert(m_raftVotedFor);
    m_raftCandidateDeadline = now + kCandidateRetryBase + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kCandidateRetryRand;

    LOG4_INFO("%s() start election term=%llu candidate(%s)", __FUNCTION__, (unsigned long long)m_raftTerm, m_raftVotedFor.c_str());

    for (const auto &peer : m_raftRemotePeers)
    {
        RaftRequestVote req;
        req.set_term(m_raftTerm);
        req.set_candidate_id(GetLabor()->GetWorkerIdentify());
        req.set_last_log_index(0);
        req.set_last_log_term(0);
        req.set_next_node_id_alloc_hint(static_cast<uint32_t>(m_uiNextNodeIdAlloc));
        const std::string body = req.SerializeAsString();
        GetLabor()->SendToCallback(this, net::CMD_REQ_RAFT_REQUEST_VOTE, body, RaftVoteCallback, peer, "", new DataStepCustom(peer));
    }
}

/*
 * RaftSendAppendEntriesToAll（Leader 心跳）：
 *   对每个 remote 发 AppendEntries（prev/commit 等为 0，空日志语义）
 *   携带 leader_next_node_id_alloc，Follower 用 max 更新本地 m_uiNextNodeIdAlloc
 */
void SessionRaftCluster::RaftSendAppendEntriesToAll()
{
    if (!m_bIsLeader || m_raftRole != CenterRaftRole::Leader)
    {
        return;
    }
    for (const auto &peer : m_raftRemotePeers)
    {
        RaftAppendEntries ae;
        ae.set_term(m_raftTerm);
        ae.set_leader_id(GetLabor()->GetWorkerIdentify());
        ae.set_prev_log_index(0);
        ae.set_prev_log_term(0);
        ae.set_leader_commit(0);
        ae.set_leader_next_node_id_alloc(static_cast<uint32_t>(m_uiNextNodeIdAlloc));
        const std::string body = ae.SerializeAsString();
        GetLabor()->SendToCallback(this, net::CMD_REQ_RAFT_APPEND_ENTRIES, body, RaftAppendCallback, peer, "", nullptr);
    }
}

/*
 * HandleRaftRequestVote（RPC 服务端）：
 *
 *   req.term < 本地 term -> 拒绝，回当前 term + 本地游标 hint
 *   req.term > 本地 term -> RaftBecomeFollower(req.term)
 *   同 term 且本机仍是 Leader -> 拒绝（不拆现任 Leader）
 *   本 term 未投票或已投给同一 candidate -> 赞成（vote_granted）；授票不改本地 m_uiNextNodeIdAlloc；延长 follower 选举超时
 *   否则 -> 拒绝
 *   最后回 voter_next_node_id_alloc_hint
 */
void SessionRaftCluster::HandleRaftRequestVote(const std::string & /*remote_identify*/, const RaftRequestVote &req, RaftRequestVoteRsp *rsp)
{
    const ev_tstamp now = GetLabor()->GetTimeStamp();
    if (req.term() < m_raftTerm)
    {
        rsp->set_term(m_raftTerm);
        rsp->set_vote_granted(false);
        rsp->set_voter_next_node_id_alloc_hint(static_cast<uint32_t>(m_uiNextNodeIdAlloc));
        return;
    }
    if (req.term() > m_raftTerm)
    {
        RaftBecomeFollower(req.term());
    }
    rsp->set_term(m_raftTerm);

    if (req.term() == m_raftTerm && m_raftRole == CenterRaftRole::Leader)
    {
        rsp->set_vote_granted(false);
        rsp->set_voter_next_node_id_alloc_hint(static_cast<uint32_t>(m_uiNextNodeIdAlloc));
        return;
    }

    if (m_raftVotedFor.empty() || m_raftVotedFor == req.candidate_id())
    {
        m_raftVotedFor = req.candidate_id();
        rsp->set_vote_granted(true);
        m_raftFollowerDeadline = now + kFollowerElectionBase + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kFollowerElectionRand;
    }
    else
    {
        rsp->set_vote_granted(false);
    }
    rsp->set_voter_next_node_id_alloc_hint(static_cast<uint32_t>(m_uiNextNodeIdAlloc));
}

/*
 * HandleRaftAppendEntries（RPC 服务端，作心跳）：
 *
 *   req.term < 本地 term -> success=false
 *   req.term > 本地 term -> BecomeFollower
 *   同 term 且本机 Candidate -> 降为 Follower（承认合法 Leader 的 RPC）
 *   记录 leader_id、刷新 last_leader_contact 与 follower 选举随机超时
 *   leader_next_node_id_alloc>0 -> max 对齐本地游标
 *   success=true
 */
void SessionRaftCluster::HandleRaftAppendEntries(const std::string & /*remote_identify*/, const RaftAppendEntries &req, RaftAppendEntriesRsp *rsp)
{
    const ev_tstamp now = GetLabor()->GetTimeStamp();
    if (req.term() < m_raftTerm)
    {
        rsp->set_term(m_raftTerm);
        rsp->set_success(false);
        return;
    }
    if (req.term() > m_raftTerm)
    {
        RaftBecomeFollower(req.term());
    }
    else if (m_raftRole == CenterRaftRole::Candidate)
    {
        m_raftRole = CenterRaftRole::Follower;
        m_bIsLeader = false;
        m_uiBeLeaderTime = 0;
    }

    m_raftLeaderId = req.leader_id();
    m_raftLastLeaderContact = now;
    m_raftFollowerDeadline = now + kFollowerElectionBase + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kFollowerElectionRand;

    if (req.leader_next_node_id_alloc() > 0)
    {
        m_uiNextNodeIdAlloc = static_cast<uint16>(std::max<uint32_t>(m_uiNextNodeIdAlloc, req.leader_next_node_id_alloc()));
        if (m_uiNextNodeIdAlloc >= static_cast<uint16>(NODE_ID_MAX))
        {
            m_uiNextNodeIdAlloc = 1;
        }
    }

    rsp->set_term(m_raftTerm);
    rsp->set_success(true);
}

/*
 * OnRaftVoteResponse（异步回调）：
 *
 *   rsp.term > 本地 -> BecomeFollower，丢弃后续逻辑
 *   非 Candidate 或 rsp.term < m_raftElectionTerm -> 陈旧响应，忽略
 *   vote_granted -> 合并 voter_next_node_id_alloc_hint（max 游标）；从 StepParam 取 peer identify 加入 votes_granted
 *   |votes| >= majority -> RaftBecomeLeader
 */
void SessionRaftCluster::OnRaftVoteResponse(net::StepParam *param, const RaftRequestVoteRsp &rsp)
{
    const ev_tstamp now = GetLabor()->GetTimeStamp();
    if (rsp.term() > m_raftTerm)
    {
        RaftBecomeFollower(rsp.term());
        return;
    }
    if (m_raftRole != CenterRaftRole::Candidate || rsp.term() < m_raftElectionTerm)
    {
        return;
    }
    if (rsp.vote_granted())
    {
        if (rsp.voter_next_node_id_alloc_hint() > 0)
        {
            m_uiNextNodeIdAlloc = static_cast<uint16>(std::max<uint32_t>(m_uiNextNodeIdAlloc, rsp.voter_next_node_id_alloc_hint()));
            if (m_uiNextNodeIdAlloc >= static_cast<uint16>(NODE_ID_MAX))
            {
                m_uiNextNodeIdAlloc = 1;
            }
        }
        auto *d = dynamic_cast<DataStepCustom *>(param);
        if (d && !d->strToNodeIdentify.empty())
        {
            m_raftVotesGranted.insert(d->strToNodeIdentify);
        }
    }
    if (m_raftVotesGranted.size() >= m_raftMajority)
    {
        RaftBecomeLeader(now);
    }
}

/*
 * OnRaftAppendEntriesResponse：
 *   仅处理「对端 term 更大」——退位 Follower（网络分区恢复后旧 Leader 发现新 term）。
 */
void SessionRaftCluster::OnRaftAppendEntriesResponse(const RaftAppendEntriesRsp &rsp)
{
    if (rsp.term() > m_raftTerm)
    {
        RaftBecomeFollower(rsp.term());
    }
}

} // namespace coor
