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
/** 冷启动 / 从未收到 Leader AE（m_raftLastLeaderContact==0）：仅用于 m_raftFollowerDeadline。
 *  0.20s + U(0,1)*0.30s → [0.20,0.50]s：在 1s Session 周期下通常 1 个 tick 内即有节点起选，随机段减轻多机同时拉票。 */
constexpr ev_tstamp kFollowerColdStartBase = 0.20;
constexpr ev_tstamp kFollowerColdStartRand = 0.30;

/** 跟主租约（与冷启动拆开）：RaftTick 中若 last_leader_contact>0，则 lease =
 *    kFollowerLeaseCenterBeatMult * max(1,center_beat) + m_raftFollowerLeaseExtra（秒，与 GetTimeStamp 一致）。
 *  mult=2：租约下限为 2 格心跳；默认 center_beat=3s 时 6s > 3s，避免「心跳未到却误判掉主」。
 *  m_raftFollowerLeaseExtra 在每次合法 AppendEntries 内重置为 1.0 + U(0,1)*0.5：固定 1s 裕量 + 至多 0.5s 抖动，
 *  吸收网络延迟、调度及 Leader 用 GetNowTime 发心跳与 Follower GetTimeStamp 判超时的步进差。 */
constexpr int kFollowerLeaseCenterBeatMult = 2;
constexpr ev_tstamp kFollowerLeaseMarginBase = 1.0;
constexpr ev_tstamp kFollowerLeaseMarginRand = 0.5;

constexpr ev_tstamp kCandidateRetryBase = 0.08;
constexpr ev_tstamp kCandidateRetryRand = 0.12;

ev_tstamp FollowerLeaseExtraFromMargins()
{
    return kFollowerLeaseMarginBase
        + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kFollowerLeaseMarginRand;
}

/** 游标在 [1, NODE_ID_MAX-1]，发满后再从 1 开始。合并两个游标不能用 std::max：
 *  例：本机已回绕到 5、对端仍是 200 时，max(5,200)=200 会错。做法：先把两边都变成 0～NODE_ID_MAX-2（游标减 1），
 *  想象 255 个格子排成一圈（每次「下一个 id」+1，到顶再绕回 0）。从**本地**那一格出发，只按这个方向一格一格数，
 *  数到**远端**那一格要经过几格，代码里就是 forwardLocalToRemote，用 mod 255 算（NODE_ID_MAX==256 时模数为 255）。
 *  若这个格数在 1～127 之间，就认为远端在发放顺序上更「靠后」，合并取远端；否则保留本地（和 TCP 比序号谁新谁旧同类）。 */
uint16_t MergeNodeIdAllocRing(uint16_t local, uint32_t remoteU32)
{
    if (remoteU32 == 0u || remoteU32 >= static_cast<uint32_t>(NODE_ID_MAX))
    {
        return (local == 0u) ? static_cast<uint16_t>(1u) : local;
    }
    const uint16_t remote = static_cast<uint16_t>(remoteU32);
    if (local == 0u)
    {
        local = 1u;
    }
    const uint32_t ring = static_cast<uint32_t>(NODE_ID_MAX - 1u);
    const uint32_t a = static_cast<uint32_t>(local) - 1u;
    const uint32_t b = static_cast<uint32_t>(remote) - 1u;
    const uint32_t forwardLocalToRemote = (b + ring - a) % ring;
    if (forwardLocalToRemote == 0u)
    {
        return local;
    }
    if (forwardLocalToRemote < (ring + 1u) / 2u)
    {
        return remote;
    }
    return local;
}

/** 将游标强制归一到有效范围 [1, NODE_ID_MAX-1]。
 *  主要用于防御性处理：Leader/follower 合并 ring 后、或外部兼容路径把游标推到 0 或 NODE_ID_MAX 的情况。
 */
uint16_t SanitizeNodeIdCursor(uint16_t cursor)
{
    if (cursor == 0u || cursor >= static_cast<uint16_t>(NODE_ID_MAX))
    {
        return 1u;
    }
    return cursor;
}

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
    m_raftFollowerLeaseExtra = 0;

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
        m_raftFollowerDeadline = now + kFollowerColdStartBase
            + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kFollowerColdStartRand;
        m_raftCandidateDeadline = now;
        LOG4_TRACE("%s() multi-node raft follower, first election after %.3f", __FUNCTION__, m_raftFollowerDeadline - now);
    }
}

// 分配当前游标指向的 id（必为 1..255），再递增游标；游标到 NODE_ID_MAX 后回绕到 1。
uint16_t SessionRaftCluster::AllocNextNodeId()
{
    m_uiNextNodeIdAlloc = SanitizeNodeIdCursor(m_uiNextNodeIdAlloc);
    const uint16_t id = m_uiNextNodeIdAlloc;
    ++m_uiNextNodeIdAlloc;
    if (m_uiNextNodeIdAlloc >= static_cast<uint16_t>(NODE_ID_MAX))
    {
        m_uiNextNodeIdAlloc = 1;
    }
    return id;
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
 *     若曾收到 Leader（last_leader_contact>0）且在租约内 -> 保持（租约 = mult*center_beat + lease_extra）
 *     否则若未到冷启动 follower_deadline -> 保持
 *     否则 -> RaftStartElection
 */
void SessionRaftCluster::RaftTick(ev_tstamp now)
{
    // 如果是单节点集群，直接返回
    if (m_raftSingleNode)
    {
        return;
    }
    // 如果当前为 Leader，直接返回
    if (m_raftRole == CenterRaftRole::Leader)
    {
        return;
    }
    // 如果当前为 Candidate，并且已经到达或超过 Candidate 超时时间，启动新一轮选举
    if (m_raftRole == CenterRaftRole::Candidate)
    {
        if (now >= m_raftCandidateDeadline)
        {
            RaftStartElection(now);
        }
        return;
    }
    // Follower：曾收到 Leader AE 则在租约内不抢选（租约随 center_beat 放大，见文件顶常量注释）
    if (m_raftLastLeaderContact > 0.0)
    {
        const uint32 beat = std::max<uint32>(1u, m_uiCenterBeat);
        const ev_tstamp lease =
            static_cast<ev_tstamp>(static_cast<uint64_t>(kFollowerLeaseCenterBeatMult) * beat) + m_raftFollowerLeaseExtra;
        if (now < m_raftLastLeaderContact + lease)
        {
            return;
        }
    }
    // 如果未曾联系过 Leader，但未到 follower_deadline，同样保持不动
    else if (now < m_raftFollowerDeadline)
    {
        return;
    }
    // 前面条件都不满足，启动选举
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
 *   携带 leader_next_node_id_alloc，Follower 用 MergeNodeIdAllocRing 与本地游标合并
 */
void SessionRaftCluster::RaftSendAppendEntriesToAll()
{
    if (!m_bIsLeader || m_raftRole != CenterRaftRole::Leader)
    {
        return;
    }
    SessionOnlineNodes *online = GetSessionOnlineNodes();
    if (online && !m_raftRemotePeers.empty())
    {
        online->BumpOnlineSnapshotSeqForRaft();
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
        if (online)
        {
            online->FillLeaderOnlineSnapshotForRaftAppend(&ae);
        }
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
    // 如果请求中的 term 小于本地 term，拒绝投票，并返回本地 term 和本地 node id alloc 游标提示
    if (req.term() < m_raftTerm)
    {
        rsp->set_term(m_raftTerm);
        rsp->set_vote_granted(false);
        rsp->set_voter_next_node_id_alloc_hint(static_cast<uint32_t>(m_uiNextNodeIdAlloc));
        return;
    }
    // 如果请求中的 term 大于本地 term，则本节点切换为 Follower，并更新本地 term
    if (req.term() > m_raftTerm)
    {
        RaftBecomeFollower(req.term());
    }
    // 返回当前（可能已更新）term
    rsp->set_term(m_raftTerm);

    // 如果 term 相等且本节点还是 Leader，拒绝投票（不拆现任 Leader），并返回游标提示
    if (req.term() == m_raftTerm && m_raftRole == CenterRaftRole::Leader)
    {
        rsp->set_vote_granted(false);
        rsp->set_voter_next_node_id_alloc_hint(static_cast<uint32_t>(m_uiNextNodeIdAlloc));
        return;
    }

    // 如果本轮未投票或者上次投票就是给本候选人，赞成投票，并延长 follower 选举超时时间
    if (m_raftVotedFor.empty() || m_raftVotedFor == req.candidate_id())
    {
        // 赞成投票，将本地已投票记录设置为本候选人（为啥选本节点: 本轮未投票或已投给本候选人，因此当前可以授票）
        m_raftVotedFor = req.candidate_id();

        rsp->set_vote_granted(true);
        // 延长 follower 选举超时时间，是因为本节点刚参与了投票，这说明它与集群的通信良好，不应过早发起新的选举造成不必要的分裂
        m_raftFollowerDeadline = now + kFollowerColdStartBase
            + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kFollowerColdStartRand;
    
    }
    else
    {
        // 否则，已经投给其他 candidate，拒绝投票
        rsp->set_vote_granted(false);
    }
    // 返回本地节点的 node id alloc 游标提示
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
    m_raftFollowerLeaseExtra = FollowerLeaseExtraFromMargins();
    m_raftFollowerDeadline = now + kFollowerColdStartBase
        + (static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0) * kFollowerColdStartRand;

    if (req.leader_next_node_id_alloc() > 0)
    {
        m_uiNextNodeIdAlloc = MergeNodeIdAllocRing(m_uiNextNodeIdAlloc, req.leader_next_node_id_alloc());
        if (m_uiNextNodeIdAlloc >= static_cast<uint16>(NODE_ID_MAX))
        {
            m_uiNextNodeIdAlloc = 1;
        }
    }

    SessionOnlineNodes *online = GetSessionOnlineNodes();
    if (online)
    {
        online->ApplyOnlineSnapshotFromLeader(req);
    }

    rsp->set_term(m_raftTerm);
    rsp->set_success(true);
}

/*
 * OnRaftVoteResponse（异步回调）：
 *
 *   rsp.term > 本地 -> BecomeFollower，丢弃后续逻辑
 *   非 Candidate 或 rsp.term < m_raftElectionTerm -> 陈旧响应，忽略
 *   vote_granted -> 用 MergeNodeIdAllocRing 合并 hint（mod 255 意义下比谁先谁后，非线性 max）；从 StepParam 取 peer identify 加入 votes_granted
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
            m_uiNextNodeIdAlloc = MergeNodeIdAllocRing(m_uiNextNodeIdAlloc, rsp.voter_next_node_id_alloc_hint());
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
