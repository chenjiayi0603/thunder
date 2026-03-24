# Center 多节点：Raft + 业务注册 — 流程与选主规则

算法步骤、状态机与经典 Raft 的差异见同目录 [Center-Raft-Election-Design.md](Center-Raft-Election-Design.md)（§14、§15）。

---

## 选主规则（选举与当选）

与 `SessionRaftCluster` 当前实现一致。

### 集群与多数派

要点：成员里必须有本机，否则各进程会误以为自己是唯一节点；当选靠票数过线，不是谁先喊谁当。

```
  配置 centers
       |
       v
  InitElection：host:port -> ip:port.0 -> m_raftClusterPeers
       |
       +-- 列表缺本机 GetWorkerIdentify() --> 补入本机再排序（防「自票假主」）
       |
       v
  n = |peers|     m_raftMajority = max(1, floor(n/2)+1)
       |
       +-- n==1 且唯一成员==本机 --> 单节点：term=1，直接 Leader，不跑选举定时
       |
       +-- 否则 --> 多节点：初始 Follower，超时后拉票
```

多数派含义：候选人集合 `m_raftVotesGranted` 里要有至少 `m_raftMajority` 张赞成票（含自己投自己那一票），才调用 `RaftBecomeLeader`。

### 何时发起选举（RaftTick）

要点：跟过主则在「跟主租约」内不抢选（租约随 `center_beat` 放大，与冷启动截止拆开）；Candidate 超时则加 term 再拉一轮。

```
              RaftTick（非 Leader、非单节点）
                        |
        +---------------+----------------+
        |                                |
  role==Candidate                  role==Follower
        |                                |
  now>=candidate_deadline?             last_leader_contact > 0 ?
   是: RaftStartElection                   |
   否: 保持                          是: now < last+跟主租约? 是:保持 否:拉选
        |                                |      租约 = mult*center_beat + lease_extra
        |                            否: now < m_raftFollowerDeadline? 是:保持 否:拉选
        |                                |      （冷启动随机截止，见下）
        +---------- RaftStartElection <--+
                    (term++, 重发 RequestVote)
```

定时常量（`SessionRaftCluster.cpp` 匿名命名空间）：

- **冷启动 / `m_raftFollowerDeadline`**（`last_leader_contact==0` 或授票时延长）：`kFollowerColdStartBase`（0.20s）+ `U(0,1) * kFollowerColdStartRand`（0.30s）→ 约 **[0.20, 0.50]s**；与 1s Session 周期配合，尽快首轮选主并打散同时拉票。
- **跟主租约**（`last_leader_contact>0`）：`kFollowerLeaseCenterBeatMult`（2）× `max(1, m_uiCenterBeat)` + `m_raftFollowerLeaseExtra`；`m_raftFollowerLeaseExtra` 在每次合法 **AppendEntries** 重置为 `kFollowerLeaseMarginBase`（1.0s）+ `U(0,1) * kFollowerLeaseMarginRand`（0.5s）。默认 `center_beat=3` 时租约约 **7～7.5s**，大于单格心跳，避免误判掉主。
- **Candidate 重试**：`kCandidateRetryBase` + 随机 `kCandidateRetryRand`。

### RaftStartElection（当候选人）

要点：term 加一、先给自己一票、清空 leader_id，再向所有 remote 并发 RequestVote；报文里的 `last_log_*` 恒为 0，不做日志新旧比较。

```
  RaftStartElection
        |
        +--> role=Candidate, m_raftTerm++, votedFor=本机
        +--> leader_id 清空, election_term=当前 term, votes={本机}
        |
        v
  对每个 remote: Send RequestVote(term, candidate_id, next_node_id_alloc_hint)
```

### HandleRaftRequestVote（投票方）

要点：过期 term 直接拒；更高 term 先降级跟票；同 term 已是 Leader 不拆台；同一 term 每人最多投一个候选人。`vote_granted=true` 只表示本机同意把票投给该候选人，不在此步修改本地 `m_uiNextNodeIdAlloc`；全集群游标在 Leader 产生后由 `AppendEntries.leader_next_node_id_alloc` 做 max 对齐。

```
  收到 RequestVote(req)
        |
        v
  req.term < 本地 term? ----是----> 拒绝，rsp 带本地 term
        |
        否
        v
  req.term > 本地 term? ----是----> RaftBecomeFollower(req.term)
        |
        v
  同 term 且本机仍是 Leader? --是--> 拒绝（不拆现任主）
        |
        否
        v
  本 term 尚未投票，或已投的就是 req.candidate_id（同一候选人）?
        |
   是 --+-- 否：本 term 已把票投给别的候选人 --> 拒绝
        |
        v
  授予：vote_granted=true，写 votedFor；不根据 req.next_node_id_alloc_hint 改本地游标
        重置 m_raftFollowerDeadline（冷启动公式 kFollowerColdStart*，推迟未跟主时的抢选）
```

### OnRaftVoteResponse（候选人收票）

要点：响应里 term 更大则立刻变 Follower；旧轮次的票扔掉；仅当 vote_granted 为真时才合并对端 `voter_next_node_id_alloc_hint`（同意票附带其本地游标，供候选人上任后发号基线）；票数凑够多数派则 `RaftBecomeLeader`。全量 Follower 仍以 Leader 心跳里的 `leader_next_node_id_alloc` 为准拉齐游标。

```
  收到 VoteRsp(rsp)
        |
        v
  rsp.term > 本地 term? ----是----> RaftBecomeFollower，结束
        |
        否
        v
  仍 Candidate 且 rsp.term >= m_raftElectionTerm? ----否----> 丢弃（陈旧票）
        |
        是
        v
  vote_granted? ----否----> 不合并游标、不计票，结束本条
        |
        是
        v
  voter_next_node_id_alloc_hint>0 则 max 合并 m_uiNextNodeIdAlloc；对端 identify 加入 m_raftVotesGranted
        |
        v
  |m_raftVotesGranted| >= m_raftMajority? ----是----> RaftBecomeLeader（leader_id=本机）
```

### 选举没凑够票时怎样「重选」

要点：没有单独的「选举失败」日志；凑不够多数派就一直是 Candidate。等到「候选人重试截止」到期，会先经过一段随机延迟再进入下一轮拉票，减轻多节点同时抢选导致的反复拆分票。

- 何时算这一轮没成主：`|m_raftVotesGranted| < m_raftMajority` 且角色仍为 Candidate。
- 怎样重选：`RaftTick` 里 `now >= m_raftCandidateDeadline` 时调用 `RaftStartElection`。`m_raftCandidateDeadline` 在每次 `RaftStartElection` 末尾被设为 `now + kCandidateRetryBase + 随机 * kCandidateRetryRand`（秒级），因此失败后会先等这段延迟，再 `term++`、票数重置为仅含本机、向所有 remote 重发 `RequestVote`。
- 其它出口：更大 `term` 或合法 `AppendEntries` 可能先把本机变成 Follower；之后由 **跟主租约**到期或（从未跟主时）**`m_raftFollowerDeadline`** 再触发 `RaftStartElection`，路径与 Candidate 超时不同。

```
  Candidate，票数 < majority
        |
        v
  RaftTick：now >= m_raftCandidateDeadline ?
        |
       否 --> 保持 Candidate，继续收 VoteRsp 或等截止（带随机退避）
        |
       是 --> RaftStartElection（随机延迟已体现在 deadline；term++，重计票，再发 RequestVote）
```

### 更大 term 与 AppendEntries

要点：任何 RPC 里只要见到更大 term，本机就降为 Follower；合法心跳能让正在竞选的节点承认已有主。

```
  AppendEntries 合法（term 步进 OK）
        |
        v
  写 m_raftLeaderId，刷新 last_leader_contact，重置 m_raftFollowerLeaseExtra 与 m_raftFollowerDeadline（跟主租约 + 冷启动字段）
        |
        +-- 若本机是同 term Candidate --> 降为 Follower（认主）
```

其它路径：`req.term` 或 `rsp.term` 大于 `m_raftTerm` 时，统一 `RaftBecomeFollower(对方 term)`。

### 与经典 Raft 的差别（选主）

要点：本实现不持久化日志，投票也不比日志新旧；AppendEntries 只做心跳和 `node_id` 游标对齐，没有真实日志匹配与 commit 索引。

- 崩溃重启：内存 Raft 状态与游标不保证和集群完全一致。
- RequestVote：`last_log_index` / `last_log_term` 不参与 grant 判定。
- AppendEntries：无 entries 一致性、无按论文的「已提交」推导；实现上在同一条 RPC 里附带业务层「在线节点全量快照」供 Follower 替换路由副本（非 Raft 日志条目）。

---

## Center 间 RPC 与 Session（字符流程）

```
[ Center 进程 A ]     [ Center 进程 B ]     [ Center 进程 C ]
       |                     |                     |
       +---------- CMD 43 RequestVote ------------>|
       |<--------- CMD 44 VoteRsp ----------------+
       |                     |                     |
       +---------- CMD 45 AppendEntries ---------->|
       |            (可带 online_nodes 全量快照)    |
       |<--------- CMD 46 AppendRsp ----------------+   (Leader 周期心跳)
       |                     |                     |
  SessionRaftCluster    SessionRaftCluster    SessionRaftCluster
  (独立 Session)        (独立 Session)        (独立 Session)
       |                     |                     |
       +---------------- term / leader_id / node_id 游标对齐 ---------+
```

---

## 业务节点 Logic / Interface

```
       |   CMD 13 NodeRegister / CMD 11 NodeReport
       v
  +------------------+
  | CmdNodeRegister  |---> SessionOnlineNodes::AddNode (表项)
  |     或           |     +--> ApplyNodeRegisterRaftOutcome(...)
  | CmdNodeReport    |           (读 SessionRaftCluster 填 errcode)
  +------------------+
              |
              v
  +----------------------+
  | SessionRaftCluster   |  RaftHasStableLeader / FillNodeReportRspRaftForResponse
  +----------------------+
              |
              v
       NodeReportRsp (errcode, current_leader_identify, raft_term, node_id)
```

### 业务侧与 Center 的连接策略（发现主 / 稳态）

- **未掌握稳定主**（冷启动、本地无缓存、或刚丢失主信息）：对配置中的**全部** Center 发送 CMD 13 / CMD 11（注册与周期性上报视产品约定），依据 `NodeReportRsp` 的 `current_leader_identify`、`raft_term` 等与 `RaftHasStableLeader` 语义收敛到当前主。
- **已掌握稳定主**：**仅向主 Center** 周期性上报/保活，减少冗余流量，且与「仅 Leader 侧权威分配 `node_id` / 在线表」一致。
- **主不可达或疑似失效**：超时、连续失败或明确错误码时**丢弃或降级**缓存的主地址，回到对**全部** Center 的探测，直到再次从回包得到稳定的 `current_leader_identify`（避免长期只打旧主）。

### Center 从节点上的路由视图（与业务连接解耦）

与上节「稳态仅向主上报」配套：**从节点上的在线/路由信息只应来自 Leader 侧的权威状态经同步（全量或等价一致的增量+基准，具体实现依产品）得到**，而不是「本机是否与某业务节点仍保持会话」的推断。

- **权威与副本**：业务 CMD 13 / CMD 11 主要在 **Leader** 上驱动 `SessionOnlineNodes`（及相下游事件）；Follower 持有的是**副本视图**。副本的增删改应与 Leader 判定一致，**不得**在 Follower 上单独按「本地有无连接」维护另一套会员真理。
- **全量/同步语义**：Leader 在 **`CMD 45 AppendEntries` 周期心跳**上挂载在线表全量快照（`RaftAppendEntries.online_nodes_seq` 非 0 且 `repeated RaftOnlineNodeEntry online_nodes` 与当前 Leader 的 `SessionOnlineNodes` 一致；空表则仅 seq+空 repeated）。Follower 在承认该 AE 后**整体替换**本地副本，拓扑滞后约为 `center_beat` 量级；**不靠**业务周期性直连本机来「猜」全局路由。旧版本 Center 不传 7/8 号字段时 `online_nodes_seq==0`，Follower 不据此改表（需升级全节点后副本才与主收敛）。
- **与从断连 ≠ 下线**：业务关闭与某 **Follower** 的 TCP（或从未长期连接该 Follower）时，**不应**据此在该 Follower 上移除该业务节点路由；否则易出现「主仍认为在线、从已删表项」的分裂。摘路由、超时剔除等一律以 **Leader 权威变更 + 复制/通知到从** 为准，Follower 只应用变更。

### Leader 上注册成功后的路由下发（仅 `IsLeadership==true`）

```
  SessionOnlineNodes::AddNodeBroadcast
       |
       +----> SendNodeNotice (CMD 15) ----> 订阅方（如 INTERFACE）
```

---

## 插件（.so）划分

| 插件 | 命令字 |
|------|--------|
| `CmdRaftRequestVote.so` | CMD 43 |
| `CmdRaftAppendEntries.so` | CMD 45 |
| `CmdNodeRegister.so` | CMD 13 |
| `CmdNodeReport.so` | CMD 11 |

---

## 选举与稳态：变量流动（每个 Center 进程内 `SessionRaftCluster`）

（代码字段名；对业务回包见 `NodeReportRsp.raft_term` / `current_leader_identify` / `node_id`）

### 本地状态变量 — 含义速查

| 变量 | 含义 |
|------|------|
| `m_raftTerm` | 当前已知任期；选举开始时 Candidate 自增；更大 term 的 RPC 会拉高并降级 |
| `m_raftElectionTerm` | 本轮拉票发起时的 term，用于 `OnRaftVoteResponse` 丢弃陈旧票 |
| `m_raftRole` | `Follower` \| `Candidate` \| `Leader` |
| `m_raftVotedFor` | 本 term 把票投给了谁（`candidate_id`）；Leader 侧同 term 拒外来票 |
| `m_raftLeaderId` | 认定的 Leader identify；Leader 上台时=本机；Follower 收 AE 时=`leader_id` |
| `m_raftVotesGranted` | 本轮选举投赞成票的对端集合（含自己）；集合大小 ≥ majority → 当选 |
| `m_uiCenterBeat` | Leader 发 AppendEntries 的间隔（秒，配置 `center_beat`）；参与跟主租约计算 |
| `m_raftFollowerDeadline` | 冷启动路径：从未跟主时最早允许拉选的时刻（`kFollowerColdStart*` 随机）；授票时同公式延后 |
| `m_raftFollowerLeaseExtra` | 每次合法 AE 重置：`kFollowerLeaseMarginBase` + 随机 `kFollowerLeaseMarginRand`；与 `2*center_beat` 相加得跟主租约 |
| `m_uiNextNodeIdAlloc` | 下一待分配 `node_id` 游标 `[1, NODE_ID_MAX)`；仅 Leader 调用 `AllocNextNodeId` 递增；其余场景与对端 hint 做 `max` 对齐 |

### 与 proto / 回包的对应

- `NodeReportRsp.raft_term` ← 本地 `m_raftTerm`（`FillNodeReportRspRaftForResponse`）
- `NodeReportRsp.current_leader_identify` ← 稳定时 `m_raftLeaderId`
- `NodeReportRsp.node_id` ← 注册/上报业务字段；新号仅 Leader 上 `AddNode` → `AllocNextNodeId`

### CMD 43 RequestVote 报文字段与本地变量

**请求**

- `req.term` = 候选方 `m_raftTerm`（已 `++` 后）
- `req.next_node_id_alloc_hint` = 候选方携带的游标（投票方授票时不用它改本地游标；候选人仅在收到 `vote_granted` 的应答里用 `voter_next_node_id_alloc_hint` 做 max）

**响应**

- `rsp.term` = 投票方当前 `m_raftTerm`（可能因 `req.term` 更高刚被拉高）
- `rsp.vote_granted`：仅表示同意选主，不隐含修改投票方 `m_uiNextNodeIdAlloc`
- `rsp.voter_next_node_id_alloc_hint` = 投票方当前 `m_uiNextNodeIdAlloc`（供候选人侧在同意票上合并）

### CMD 45 AppendEntries 报文字段与本地变量（心跳）

**请求**

- `req.term` / `req.leader_id` → Follower 更新 `m_raftLeaderId`、`m_raftLastLeaderContact`、重置 `m_raftFollowerLeaseExtra` 与 `m_raftFollowerDeadline`（冷启动公式）
- `req.leader_next_node_id_alloc` → Follower：`m_uiNextNodeIdAlloc = max(本地, 字段)`

**响应**

- `rsp.term` → Leader 若见更大 term → `RaftBecomeFollower`

---

## 多节点：一次选举中变量怎么动（简化双机 A 选、B 投）

**初始（`InitElection` 后）**

- A、B：`m_raftTerm=0`，`role=Follower`，`m_raftLeaderId` 空，`m_raftVotedFor` 空，`m_uiNextNodeIdAlloc` 各自初值（通常 1）

**A：`RaftStartElection`**

- `m_raftTerm`: 0 → 1
- `m_raftRole`: `Candidate`
- `m_raftVotedFor` = A_self
- `m_raftLeaderId.clear()`
- `m_raftElectionTerm` = 1
- `m_raftVotesGranted` = { A_self }
- 向 B 发 `RequestVote(term=1, next_node_id_alloc_hint=A的游标)`

**B：`HandleRaftRequestVote`**

- `req.term(1) > m_raftTerm(0)` → `RaftBecomeFollower(1)`：`m_raftTerm=1`，`role=Follower`，`m_raftVotedFor` 清空（`BecomeFollower`）
- 本 term 未投或投给同一 candidate → grant：`m_raftVotedFor` = A；B 的 `m_uiNextNodeIdAlloc` 不因 `req.next_node_id_alloc_hint` 改变
- `rsp`: `term=1`，`vote_granted=true`，`voter_next_node_id_alloc_hint`=B 的游标

**A：`OnRaftVoteResponse`**

- `rsp.term` 与选举轮次一致；`vote_granted` 为真 → `max` 合并 B 的 `voter_next_node_id_alloc_hint` 到 A 的 `m_uiNextNodeIdAlloc`，并把 B 加入 `m_raftVotesGranted`
- `|votes| >= majority` → `RaftBecomeLeader`：`m_raftRole=Leader`，`m_raftLeaderId=A_self`，`m_raftTerm` 仍为 1（本轮未再 `++`）

---

## 稳态：Leader 心跳带游标

- 每 `center_beat`，Leader 对所有 Follower：`AppendEntries(term, leader_id, leader_next_node_id_alloc = Leader.m_uiNextNodeIdAlloc)`
- Follower `HandleRaftAppendEntries`：`m_raftLeaderId = req.leader_id()`，`m_uiNextNodeIdAlloc = max(本地, req.leader_next_node_id_alloc)`（与 Leader 对齐「下一号」）
- 仅当本机 `IsRaftLeader` 且业务 `AddNode` 需新号：`AllocNextNodeId()` 读当前游标返回 id，再本地 `++m_uiNextNodeIdAlloc`（Follower 从不在此路径发号）

---

## 更大 term 时的 term / 角色（任意 RPC 路径）

若 `req.term` 或 `rsp.term >` 本地 `m_raftTerm`：

- `RaftBecomeFollower(对方 term)`：`m_raftTerm` 更新为较大值；`role=Follower`；`m_raftVotedFor` 清空；`m_bIsLeader=false`
- 之后由 `AppendEntries` 再写入 `m_raftLeaderId`，或再次超时进入新一轮选举

---

## 单节点退化（无 remote）

- `InitElection` 后：`m_raftTerm=1`，`role=Leader`，`m_raftLeaderId=self`
- 不跑 `RaftStartElection` / `RequestVote`；心跳仍可按 `center_beat` 发（无对端则空转）
