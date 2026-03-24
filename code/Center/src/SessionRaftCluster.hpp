/*******************************************************************************
 * Project:  Center
 * @file     SessionRaftCluster.hpp
 * @brief    Center 间 Raft 选主与 node_id 游标同步（独立 Session，与 SessionOnlineNodes 并列）
 *
 * 职责概要：
 *   - 多 Center 进程间：RequestVote / AppendEntries（空日志）驱动 term 与 Leader 收敛。
 *   - Leader 经 AppendEntries 携带 leader_next_node_id_alloc，Follower 与本地游标用 MergeNodeIdAllocRing 合并（mod 255 语义，见 cpp 注释）；
 *     同一条 AppendEntries 可携带在线节点全量快照（online_nodes_seq + online_nodes），Follower 整体替换 SessionOnlineNodes 副本。
 *     候选人仅在收到 vote_granted 的 VoteRsp 时合并 voter_next_node_id_alloc_hint（与完整 Raft 日志复制不同，属业务层约定）。
 ******************************************************************************/
#ifndef SRC_SESSIONRAFTCLUSTER_HPP_
#define SRC_SESSIONRAFTCLUSTER_HPP_

#include "protocol/oss_sys.pb.h"
#include "Comm.hpp"

namespace coor
{

/** 有效 Logic node_id 为 [1, NODE_ID_MAX-1]（即 1..255）；NODE_ID_MAX 为排他上界，游标递增到此后回绕到 1。 */
#define NODE_ID_MAX (256)

/** Raft 角色：Follower 跟随；Candidate 拉票；Leader 发心跳并广播 AppendEntries。 */
enum class CenterRaftRole : uint8_t
{
    Follower = 0,
    Candidate = 1,
    Leader = 2
};

/**
 * @brief SendToCallback 投票 RPC 的 Step 上下文，用于在 OnRaftVoteResponse 中识别「哪一票来自哪个 peer」。
 */
struct DataStepCustom : public net::StepParam
{
    explicit DataStepCustom(const std::string &strNodeIdentify) : strToNodeIdentify(strNodeIdentify)
    {
        LOG4_TRACE("%s() strToNodeIdentify(%s)!", __FUNCTION__, strNodeIdentify.c_str());
    }
    ~DataStepCustom() { LOG4_TRACE("%s() strToNodeIdentify(%s)!", __FUNCTION__, strToNodeIdentify.c_str()); }
    std::string strToNodeIdentify;
};

/**
 * @brief Center 集群 Raft 状态机所在 Session（配置来自 CenterCmd.json 的全局 Session 槽位）。
 *
 * 定时驱动：Timeout() 周期调用 RaftTick；Leader 按 center_beat 周期向各 Follower 发 AppendEntries。
 * 消息入口：CmdRaftRequestVote / CmdRaftAppendEntries 插件转发到本类的 Handle*。
 */
class SessionRaftCluster : public net::Session
{
public:
    SessionRaftCluster(const std::string &strSessionId, ev_tstamp dSessionTimeout, const std::string &strSessionClass)
        : net::Session(strSessionId, dSessionTimeout, strSessionClass)
    {
    }
    ~SessionRaftCluster() override = default;
    /** 与 Center.json 中 session_class 一致，用于 Loader 创建实例。 */
    static const std::string SessionClass() { return std::string("coor::SessionRaftCluster"); }

    /** 读取 center_beat、centers 列表，一次性初始化 peer 集合与单/多节点初始角色。 */
    bool Init(const util::CJsonObject &conf) override;
    /** 会话定时器：推进 Raft 时钟、Follower 超时竞选、Leader 心跳 AppendEntries。 */
    net::E_CMD_STATUS Timeout() override;

    /**
     * @param oCenter CenterCmd 中 centers 数组（元素为 worker 名，内部会拼 ".0" 为连接 identify）。
     *              空数组则视为仅本机单节点集群。
     */
    void InitElection(const util::CJsonObject &oCenter);

    /** 当前进程是否为 Raft Leader（与 m_raftRole==Leader 一致维护）。 */
    bool IsRaftLeader() const { return m_bIsLeader; }
    /** 最近一次成为 Leader 的微秒时间戳（util::GetMicrosecond），非 Leader 时为 0。 */
    uint64_t GetUiBeLeaderTime() const { return m_uiBeLeaderTime; }

    /** 处理对端发来的 RequestVote：term 比较、是否投票、顺带对齐 node_id 游标。 */
    void HandleRaftRequestVote(const std::string &remote_identify, const RaftRequestVote &req, RaftRequestVoteRsp *rsp);
    /** 处理对端发来的 AppendEntries：承认 Leader、刷新选举超时、同步 leader_next_node_id_alloc。 */
    void HandleRaftAppendEntries(const std::string &remote_identify, const RaftAppendEntries &req, RaftAppendEntriesRsp *rsp);
    /** 异步收到 RequestVote 响应：累计票数，达多数则 RaftBecomeLeader。 */
    void OnRaftVoteResponse(net::StepParam *param, const RaftRequestVoteRsp &rsp);
    /** 异步收到 AppendEntries 响应：若对端 term 更高则退位为 Follower。 */
    void OnRaftAppendEntriesResponse(const RaftAppendEntriesRsp &rsp);

    /** 按 errcode 与当前是否已有稳定 Leader，填充 NodeReportRsp 的 raft_term / current_leader_identify。 */
    void FillNodeReportRspRaftForResponse(NodeReportRsp &rsp, uint32_t errcode) const;
    /** 非 Candidate 且已记录 m_raftLeaderId 时认为集群视角上「有已知 Leader」。 */
    bool RaftHasStableLeader() const;
    uint64_t RaftCurrentTerm() const { return m_raftTerm; }

    /** 构造 ModuleAdmin「show center」等用的 JSON：各 peer 的 identify / leader / online。 */
    void GetRaftCenterJson(util::CJsonObject &oCenter) const;

    /** 仅应由当前 Raft Leader 调用：分配下一个 Logic node_id（1..NODE_ID_MAX-1 循环）。 */
    uint16_t AllocNextNodeId();

    /** 外部（如旧接口兼容）显式切为 Leader 状态；一般选主由 Raft 内部完成。 */
    void BeLeader();
    /** 放弃 Leader 标记，角色降为 Follower（不修改 term）。 */
    void RelievedLeader();

private:
    /** Follower/Candidate：检查是否超时，触发或重试 RaftStartElection。 */
    void RaftTick(ev_tstamp now);
    /** Leader：向所有远端发 AppendEntries（空条目），携带 leader_next_node_id_alloc。 */
    void RaftSendAppendEntriesToAll();
    /** 升为 Candidate、term++、给自己一票、向各 peer 发 RequestVote。 */
    void RaftStartElection(ev_tstamp now);
    /** 同步到指定 term 的 Follower 语义：清 votedFor、下台。 */
    void RaftBecomeFollower(uint64_t term);
    /** 本机成为 Leader：写 m_raftLeaderId、时间戳，清空投票状态。 */
    void RaftBecomeLeader(ev_tstamp now);

    bool m_configApplied = false;           ///< Init 仅应用一次
    uint32 m_uiCenterBeat = 3;              ///< Leader 心跳（AppendEntries）间隔（秒）
    uint32 m_uiLastSendCenterBeat = 0;      ///< 上次按 center_beat 发心跳的 server 时间
    uint32 m_uiServerTime = 0;              ///< GetNowTime 缓存

    bool m_bIsLeader = false;               ///< 业务侧「是否 Leader」快速判断
    uint64 m_uiBeLeaderTime = 0;            ///< 成为 Leader 的微秒时间

    uint16 m_uiNextNodeIdAlloc = 1;         ///< 下一待分配 node_id（Leader 分配；Follower 经 RPC 按 mod 255 规则与主对齐游标）

    std::vector<std::string> m_raftClusterPeers; ///< 全体 Center worker identify，有序
    std::vector<std::string> m_raftRemotePeers;  ///< 除本机外的 peers
    size_t m_raftMajority = 1;                   ///< 选票所需最少票数
    bool m_raftSingleNode = true;                ///< 仅本机在成员表中

    uint64_t m_raftTerm = 0;
    std::string m_raftVotedFor;             ///< 本 term 已投给的 candidate（空表示未投）
    CenterRaftRole m_raftRole = CenterRaftRole::Follower;
    std::string m_raftLeaderId;             ///< 当前认为的 Leader identify
    ev_tstamp m_raftFollowerDeadline = 0;   ///< 冷启动/未跟主：首次允许拉选的随机时刻（与跟主租约无关）
    ev_tstamp m_raftFollowerLeaseExtra = 0; ///< 每次合法 AE 重置的随机裕量（秒），与 center_beat 倍数相加得跟主租约
    ev_tstamp m_raftCandidateDeadline = 0;  ///< Candidate 拉票超时后重试
    ev_tstamp m_raftLastLeaderContact = 0;  ///< 上次收到合法 Leader 消息的时间
    ev_tstamp m_raftLastAppendSend = 0;     ///< 上次发 AppendEntries 的时间戳（预留）

    uint64_t m_raftElectionTerm = 0;        ///< 本轮竞选发起时的 term，过滤陈旧响应
    std::unordered_set<std::string> m_raftVotesGranted; ///< 本轮已投赞成票的对端 identify
};

/**
 * @brief 从全局配置取本 Worker 的 SessionRaftCluster 单例（CenterCmd.json 第 2 个 session，下标 1）。
 * @return 未配置或未加载时可能为 nullptr，调用方需判空。
 */
inline SessionRaftCluster *GetSessionRaftCluster()
{
    /** 与 SessionOnlineNodes 同为 1s：RaftTick 用 GetTimeStamp() 判超时，降低定时器频率 */
    return net::GetGlobalConfigSession<SessionRaftCluster>("CenterCmd.json", 1.0);
}

} // namespace coor

#endif
