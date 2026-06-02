# NuRaft 接入 Center 共识层 — 设计方案

> 日期: 2026-06-03
> 状态: 设计 / 待评审(未落地)
> 选型依据: [center_raft_implementation_options.md](./center_raft_implementation_options.md)(为何选 NuRaft 而非手写/braft/etcd)
> 一句话: **用 NuRaft 替换 Center Worker 内自研的简化 Raft(`SessionRaftCluster` + `CmdRaft*` 插件),把选主 / node_id 分配 / 在线表 / 配置变更升级为「真正的 Raft 日志复制 + 持久化」,消除自研 Raft 与 leader lease 的正确性负债;节点内 `Loader → shm → Worker` 一跳与 `CenterConnector` 客户端侧完全不动。**

---

## 1. 目的(要达到的目标)

| # | 目标 | 现状痛点 | 达成判据 |
|---|------|---------|---------|
| G1 | **消除自研 Raft 正确性负债** | `SessionRaftCluster` 是简化 Raft(空日志 AppendEntries + 游标合并),无完整日志复制/持久化/快照 | 选主/复制/持久化/快照全由 NuRaft 承担,自研 Raft 下线 |
| G2 | **node_id 分配升级为强一致** | `MergeNodeIdAllocRing`(mod 255 游标合并)是业务近似,非共识 | node_id 分配是一条 Raft 日志,apply 全副本一致,**杜绝碰撞** |
| G3 | **在线表/配置强一致复制** | 在线表靠 AppendEntries 搭快照替换 | 注册/摘除/配置变更均为日志 entry,状态机 apply,全副本一致 |
| G4 | **选主 + leader lease 外包** | 自研选举 + 在写的 `raft_leader_lease_design.md`(时钟漂移/租约窗口自担) | 选举/lease read 由 NuRaft 原生承担,**leader lease 自研设计作废** |
| G5 | **日志持久化 + 崩溃恢复** | 简化 Raft 无持久日志,Worker 重启即丢状态 | Raft 日志/快照/srv_state 落盘,Worker 重启后回放 + 追平 |
| G6 | **不破坏 shm 零跳直推** | — | `Loader → shm → Worker` 一行不改,Worker 读配置仍本地 shm 纳秒级 |
| G7 | **不侵入业务节点** | — | `CenterConnector`(业务 Manager→Center 客户端侧)接口/协议不变 |
| G8 | **不引入第二调度运行时** | — | 用 NuRaft 默认 ASIO(线程池),不引 brpc/bthread,不与 libev + StepCo20 冲突 |
| G9 | **可切换/可回退** | — | 配置 `consensus_engine: self|nuraft`,启动期工厂选择 |

**非目标**(本期不做): 替换 `CenterConnector`(那是 etcd 路线);Multi-Raft;运行时动态扩 Center 节点数(成员变更 API 预留不强求)。

---

## 2. 设计总览

### 2.1 NuRaft 落点与传输

- **落点**: Center 的**单 Worker 进程**(`Center.json: process_num=1`)。NuRaft 实例单 Worker 独占。
- **传输**: **NuRaft 自带 ASIO 传输**,在 Center 节点间开独立 Raft 端口(各节点 `conf/conf2/conf3` 各配自己的 peer + 端口)。Center 专用进程多开一个端口零负担,用 NuRaft 原生设计、集成代码最少。
  - ⟹ `CmdRaftAppendEntries` / `CmdRaftRequestVote` **不再需要**(Raft 消息不再经 Thunder 的 Cmd 系统)。

### 2.2 库接入 + ConsensusEngine 抽象(无 dlopen)

- **NuRaft 本体 = 普通第三方动态库,跟 libev 一样处理**:
  - `code/3party/NuRaft/` 子模块(锁 **v3.0.0**),走 `thirdparty_deploy` 编出 `libnuraft.so` 放 `deploy/3lib/`;
  - `cmake/ThunderCommon.cmake` 链接行加 `nuraft`(`-lnuraft`),与 `ev` 同列(`ThunderCommon.cmake:146`);
  - ASIO 复用已有 standalone Asio(`AsioUringIoBackend` 在用),**版本须统一**,防 ODR。
- **ConsensusEngine = 编译期工厂,不 dlopen**(引擎启动选一次、永不热卸,dlopen 热插拔对它无价值,徒增复杂度):

```
ConsensusEngine (抽象, code/Net/include/labor/ConsensusEngine.hpp, 仿 CenterConnector.hpp)
  ├─ SelfRaftConsensusEngine    ── 现有 SessionRaftCluster 包一层(内建, 回退用)
  └─ NuRaftConsensusEngine      ── 内含 NuRaft, 静态编进 Center, 链 libnuraft.so

Center.json: "consensus_engine": "self" | "nuraft"   // 启动期工厂 new 对应实现
```

两实现都编进 Center 二进制,工厂按配置选,**无 dlopen、无符号隐藏**。

### 2.3 接口(最小集)

```cpp
class ConsensusEngine {
public:
  virtual ~ConsensusEngine() = default;
  virtual bool     Init(const Conf&, ApplyCallback, LeaderChangeCallback) = 0;
  virtual uint64_t Propose(const LogEntry&) = 0;   // 返回 request_id; apply 完成经回调带回结果(异步)
  virtual bool     IsLeader() const = 0;
  virtual std::string LeaderId() const = 0;
  virtual void     Shutdown() = 0;
};
```

`LogEntry` 类型(状态变更全部走日志): `AllocNodeId` / `RegisterNode` / `DeregisterNode` / `SetConfig`。

---

## 3. 改动全景(6 块)

```
① ConsensusEngine 抽象      —— 编译期工厂, 不 dlopen
② CenterStateMachine        —— NuRaft state_machine, 三类日志 apply
③ 注册流同步→异步           —— CmdNodeRegister/Report 改 StepCo20 协程 (最大改动)
④ node_id propose→apply→唤醒 —— 跨 NuRaft线程/libev线程 完整时序
⑤ 在线表/配置走日志          —— SessionOnlineNodes/StepSetConfig 改 apply 驱动
⑥ 选主接管 + 删自研传输      —— NuRaft 接管选举, 删 CmdRaft*/SessionRaftCluster 机制
```

---

## 4. ② CenterStateMachine(NuRaft 状态机)

实现 `nuraft::state_machine`,核心是 `commit`:

```cpp
// NuRaft 线程上调用; 所有副本按相同顺序执行 → 强一致
ptr<buffer> CenterStateMachine::commit(ulong log_idx, buffer& data) {
  LogEntry e = Decode(data);          // AllocNodeId / Register / Deregister / SetConfig
  ApplyResult r = ApplyBusiness(e);   // 纯计算: 算定 node_id / 改内存副本
  // 关键: 不在此碰 SessionOnlineNodes(那是 libev 线程的), 投递回 libev 线程落地 + 唤醒协程
  labor_->PostToEventLoop([=]{ OnApplied(log_idx, e, r); });   // ← 见 §6 时序
  return EncodeResult(r);
}
ptr<snapshot> create_snapshot(...);   // 在线表 + node_id 游标 + 配置 全量快照
void apply_snapshot(snapshot& s);     // 从快照恢复
```

- node_id 由 leader 在 `Propose` 时就**预定好建议值**写进 entry,所有副本 apply 同一个值 → 真共识(替掉现在各自 mod 255 合并)。
- 快照交给 NuRaft 做日志压缩(见 §7 日志保存)。

---

## 5. ③ 注册流同步→异步(最大改动)

**现状**(`CmdNodeRegister.cpp:61`,全同步):
```
AnyMessage: parse → AddNode()[内部 AllocNextNodeId 瞬时] → SendToClient   // 一气呵成
```

**改后**(走共识,毫秒级,必须异步):
```
AnyMessage: parse
  → engine->Propose(RegisterNode{...})   // 立刻返回 request_id, 不阻塞 libev
  → 挂起 Step 协程, 存 (request_id → MsgShell/连接) 到 pending 表
  → return STATUS_CMD_RUNNING            // 不回包, 占住连接
  ...(毫秒后 NuRaft 多数派确认 + apply)...
  → OnApplied 唤醒: 用 request_id 找回 MsgShell → SendToClient(node_id)
```

**复用现有异步范式,非造轮子**: `coro/ThreadPoolAwaitable.hpp` 的 `RunOnThreadPool` 已是"挂起协程→别线程干活→`PostToEventLoop` 回主线程恢复",把"线程池干活"换成"等 NuRaft apply"即可。`TestHelloPoolCpu`(CLAUDE.md 冒烟项)就是这套的现成验证。

`CmdNodeReport`(心跳)同理,但**纯 liveness 心跳不 Propose**(不过共识),仅 leader 本地刷租约时间戳;只有成员状态变化才 Propose(写放大控制)。

---

## 6. ④ node_id 完整时序(跨线程关键一环)

```
libev线程(Worker)            NuRaft线程(ASIO)             其他Center副本
─────────────                ─────────────                ─────────────
CmdNodeRegister
  Propose(AllocNodeId,
          hint=下一空闲id)
  挂起协程, 存 pending  ───►  append 日志(落盘) 复制 ──────►  收到, 持久化
                            等多数派 ack  ◄───────────────  ack
                            commit → state_machine::
                              commit(): 算定 node_id
                            PostToEventLoop(OnApplied)
  ◄── ev_async 唤醒 ─────────┘
  OnApplied:
    在线表落地 node_id
    pending 取回 MsgShell
    SendToClient(node_id)        (各副本也各自 commit → 在线表一致)
```

- 跨线程**只有一个方向**: NuRaft 线程 → `Labor::PostToEventLoop` → libev 线程。反向不存在(libev 线程调 `Propose` 走 NuRaft 线程安全 API)。
- **跨线程交接基础设施已存在**: `Labor::PostToEventLoop`(`Labor.cpp:660-727`,`ev_async` + mutex 队列)就是"别线程 → libev 主线程"的线程安全投递,已在生产用。`LibevHandoff` 不用新写,直接复用。
- `SessionOnlineNodes` **始终只被 libev 线程改**(都在 OnApplied 里),**无需加锁**。

---

## 7. ⑤ 在线表 / 配置走日志

| 改动点 | 现状 | 改后 |
|---|---|---|
| `SessionOnlineNodes.cpp:128` `AllocNextNodeId()` | 本地游标自增 | 删,node_id 来自 §6 apply 结果 |
| `SessionOnlineNodes` 副本同步 | AppendEntries 搭快照替换 | 注册/摘除经 apply 落地,副本天然一致 |
| `ModuleAdmin/StepSetConfig.cpp` | 直接写配置→下发 | `Propose(SetConfig)` → apply 后再走**现有下发链路** |
| 路由/配置 → shm | version++ / 半包保护 | **完全不变**(apply 后调用现有写 shm 逻辑) |

---

## 8. ⑥ 选主接管(NuRaft 接管选举,自研全删)

### 8.1 删除的自研选主机制

`SessionRaftCluster` 里以下**全去掉**(NuRaft 内部自带):

| 删除项 | 现状作用 |
|---|---|
| `RaftStartElection` / `RaftBecomeLeader` / `RaftBecomeFollower` | 竞选/升主/退位 |
| `RaftTick` + `Timeout()` 选举超时驱动 | 自推 Raft 时钟 |
| `RaftSendAppendEntriesToAll`(空日志心跳) | 自研心跳 |
| `m_raftTerm` / `m_raftVotedFor` | 任期/投票状态 |
| `HandleRaftRequestVote` / `HandleRaftAppendEntries` | 选主 RPC 处理 |
| `CmdRaftRequestVote` / `CmdRaftAppendEntries` 插件 | 选主 RPC 入口 |
| `MergeNodeIdAllocRing`(mod 255 游标合并) | 借选主 RPC 捎带 node_id 游标 |

### 8.2 依赖"是否 leader"的调用点 → 改接

| 调用点 | 现状 | 改后 |
|---|---|---|
| `SessionOnlineNodes.cpp:340-341,348,357` | `raft->IsRaftLeader()` | `engine->IsLeader()` |
| `CmdNodeRegister/Report` `ApplyNode*RaftOutcome` | `RaftHasStableLeader()` | `engine->LeaderId() != ""` |
| `FillNodeReportRspRaftForResponse` | 填 `raft_term`/`current_leader_identify` | 从 `engine` 取 term + LeaderId |
| node_id 分配前置 | 仅 leader | 仅 `engine->IsLeader()` 时 Propose |
| leader 租约时间戳 `GetUiBeLeaderTime()` | 自研维护 | NuRaft `on_become_leader` 回调里记 |

### 8.3 leader 变化通知 + 重定向语义

- NuRaft 在**自己线程**经回调(become leader/follower)通知角色变化 → `PostToEventLoop` 更新 libev 线程的缓存量(`m_isLeader` / `m_leaderId` / `m_beLeaderTime`)。**所有 leader 查询读缓存,不跨线程读 NuRaft。**
- **重定向语义保留**: 非 leader 收注册 → `engine->IsLeader()==false` → 填 `engine->LeaderId()` → 业务节点重试 leader(行为与现状对齐,业务无感;不用 NuRaft 的 auto-forward,改动更小)。

### 8.4 行为差异

| | 自研现状 | NuRaft |
|---|---|---|
| 换主收敛 | `center_beat=2~3s`,换主约 7~8.5s | 选举超时可配,通常更快可控 |
| 单节点引导 | `m_raftSingleNode` 立即成 leader | 单节点集群也立即成 leader,需配初始成员表 |
| leader lease 读 | 自研 `raft_leader_lease_design.md`(T2,在做) | **NuRaft 自带 leadership expiration / lease read → 自研设计作废** |

---

## 9. 日志保存(Raft 日志持久化)

NuRaft 需要三块持久化,都落 Center 本地磁盘(各节点独立):

| 组件 | NuRaft 接口 | 存什么 | 设计 |
|---|---|---|---|
| **日志存储** | `nuraft::log_store` | Raft 日志条目(append-only) | 文件分段(segment)+ 索引;**append 必须 fsync 后才 ack**(durability) |
| **集群/节点状态** | `nuraft::state_mgr` | `cluster_config` + `srv_state`(term/voted_for) | 小文件,变更即 fsync |
| **状态机快照** | `state_machine::create_snapshot` | 在线表 + node_id 游标 + 配置 全量 | 周期/按条数触发,落盘后截断日志(压缩) |

- **磁盘位置**: `deploy/Center/data/raft/`(各节点 `conf/conf2/conf3` 对应独立 data 目录,3 节点互不覆盖)。
- **log_store 实现**: NuRaft 自带的是内存版(测试用),生产需**持久 log_store**。两选:① 移植 NuRaft 示例/ClickHouse Keeper 的 changelog 式文件 log_store;② 自写简单文件分段 log_store(Center 状态小,日志量低,实现可很轻)。**推荐 ①**(少造轮子)。
- **崩溃恢复**: Worker 崩溃 → Manager `RestartWorker` 拉起新 Worker → NuRaft 从磁盘 load `日志 + 快照 + srv_state` → 重放 + 向 leader 追平(catch-up)。**data 目录在磁盘,跨 Worker 重启天然存活。**
- **日志膨胀控制**: 快照间隔(如每 N 条 / 每 T 秒)+ 压缩截断。Center 状态极小,快照便宜,日志可保持很短。
- ⚠️ **与 shm 无关**: 这是 Center **副本间共识日志**的落盘,跟业务节点的配置 shm 是两回事,不混。

---

## 10. 日志分析(Raft 共识日志本身)

> 这里指 **NuRaft 的 Raft 共识日志**(那些 log entry),不是应用层 log4cplus。即:如何查看、审计、校验落盘的 Raft 日志。

### 10.1 日志条目结构

每条 Raft 日志 = `{log_index, term, type, payload}`,`type` 是本设计的业务 entry:

| type | payload | 用途 |
|---|---|---|
| `AllocNodeId` | node_identify + 分定的 node_id | **node_id 分配审计**:每条即一次发号记录 |
| `RegisterNode` / `DeregisterNode` | node_info / node_id | 成员变更审计 |
| `SetConfig` | path + content + version | 配置变更审计 |

> Raft 日志天然是**有序、强一致、不可篡改的操作流水**——node_id 分给了谁、配置何时改的,全可从日志回溯。这是现状"游标合并"给不了的审计能力。

### 10.2 日志 dump / 审计

- Admin 命令 / 离线小工具 dump 指定 `log_index` 区间的 entry(index/term/type/payload 摘要),用途:
  - **node_id 分配回溯**:扫所有 `AllocNodeId`,看谁在哪个 index/term 分到哪个 id,排查碰撞/重复;
  - 配置变更历史;复制空洞排查(index 是否连续)。
- Center 状态小、日志短,dump 轻量。

### 10.3 副本一致性校验

- 同一 `log_index` 在各 Center 节点的 `term` 与内容**必须一致**(Raft Log Matching)。提供工具比对 3 节点日志,验证无分叉。
- Admin 页(`ModuleAdmin`,`/admin show center`)暴露复制进度指标:`commit_index`、`last_log_index`、`last_snapshot_index`、各 peer `match_index` → 一眼看出"复制到哪了、哪个 follower 落后"。

### 10.4 日志与快照的关系(分析时必须结合)

- 压缩(compaction)后,快照之前的日志被截断 → **完整状态 = 最近快照 + 快照后的日志**。
- 分析历史时:先 `apply_snapshot` 基线,再回放快照后 entry,才是全貌。dump 工具需同时给出 `last_snapshot_index`。

### 10.5 (辅助)NuRaft 内部运行日志 → log4cplus

- 实现 `nuraft::logger` 把 NuRaft 内部事件(选举/term 变更/append/commit)路由进 log4cplus(单独 category + 级别映射),供排查**选主抖动/复制延迟**。
- 这是辅助调试手段,与上面"分析 Raft 日志 entry"是两回事,别混。

---

## 11. 硬约束与不动的部分

### 硬约束
- ⚠️ **Center `process_num` 必须 == 1**: Worker 托管 Raft,多 Worker = 多 Raft 成员抢身份。配置层加启动断言: `consensus_engine=nuraft && process_num!=1 → 启动报错`。
- **NuRaft 锁 v3.0.0**(或含近期并发修复的 commit),勿用浮动 master(选型文档 §5: 2026 仍有并发 issue)。
- **ASIO 版本统一**: Center 已用 standalone Asio,libnuraft.so 与主程序 Asio 版本须一致(与 libev 同样动态链)。

### 完全不动
- `CenterConnector` 及 `TcpCenterConnector`(业务 Manager → Center 客户端侧): 接口/协议/业务节点全部无感。
- `Loader → shm → Worker` 配置/路由下发: version++ 原子递增、先写 blob 再写 len、半包保护一行不改。
- 业务节点(Interface/Logic/Hello)的 Worker/插件/shm 读取逻辑。

---

## 12. 渐进路线 + 回退

每步在 `ConsensusEngine` 抽象层隔离,配置可切回 `self`:

1. **P0 抽象层**(纯重构,零行为变化): 新增 `ConsensusEngine`,把现有自研 Raft 包成 `SelfRaftConsensusEngine`。验证 `ctest 全量 + E2E` 全绿。
2. **P1 注册流异步化**(不引 NuRaft): `CmdNodeRegister/Report` 改 Step 协程,`AllocNextNodeId` 仍走 self,但走一遍"挂起→`PostToEventLoop` 唤醒→回包"。**先验证异步链路,解耦最大风险**。验证: 注册 E2E 通过,node_id 正确,延迟可接受。
3. **P2 NuRaft 选主**: `code/3party/NuRaft` 子模块 + CMake 动态链 `libnuraft.so`;`NuRaftConsensusEngine` 实现选主 + 空状态机 + log_store/state_mgr 落盘;`consensus_engine` 切换。验证: 3 Center 选主收敛,与 self 对拍。
4. **P3 node_id 走真日志**(G2 核心): `Propose(AllocNodeId)` → apply → `PostToEventLoop` 恢复 P1 协程。验证: **双实例混沌对拍,网络分区下零 node_id 碰撞**。
5. **P4 在线表 + 配置走日志**(G3): 注册/摘除/配置转日志 entry,apply 对接现有 shm 下发。验证: 在线表全副本一致;配置 shm 下发不变(G6)。
6. **P5 可观测 + 持久化加固**: NuRaft logger 接 log4cplus(§10.1)、Raft 指标接 Admin(§10.2);崩溃恢复测试(Worker kill → 回放追平)。
7. **P6 混沌 + 下线自研**: TSan + 故障注入(分区/崩溃重启/换主);稳定后删 `SessionRaftCluster` + `CmdRaft*`。

---

## 13. 风险

| 风险 | 缓解 |
|------|------|
| 注册流同步→异步重写 | **P1 先不引 NuRaft 单独打通**,复用 `RunOnThreadPool` 现成范式 |
| NuRaft 自身并发 bug(2026 仍有 issue) | 锁 v3.0.0;集成后自跑 TSan + 故障注入(CLAUDE.md 并发改动 TSan 必跑) |
| 跨线程交接竞态 | 复用 `Labor::PostToEventLoop`(成熟);单向投递,不反向访问 |
| 同步 node_id 阻塞协程 | Propose 异步,经 Step 协程挂起/恢复,不阻塞 libev |
| log_store 持久化正确性(fsync/恢复) | 移植成熟实现(ClickHouse Keeper changelog 式);崩溃恢复专项测试 |
| Asio 符号/版本冲突 | 与 libev 同样动态链,统一版本;CI 加 ODR 检查 |
| 注册延迟 µs→ms(过共识) | 心跳不走共识;注册本就低频,可接受 |
| 构建变重(`-j1` 磁盘瓶颈) | NuRaft 自包含 ~2.5 万行 + Asio 已有,远轻于 braft+brpc;子模块按需编译 |

---

## 14. NuRaft Raft 原理设计分析 + 业务需求匹配

> 评估 NuRaft 的 Raft 机制能否撑住 Center 的真实需求,逐条对照,并指出唯一需要自己设计的点(node_id 槽位复用)。

### 14.1 NuRaft 的 Raft 机制(原理要点)

| 机制 | NuRaft 实现 | 对 Center 的意义 |
|---|---|---|
| **选举** | 随机选举超时 + **pre-vote**(防分区节点 term 膨胀)+ **priority election**(可设节点优先级/排除选举)+ leadership expiry(失去 quorum 自动退位) | 单写者语义的来源;pre-vote 避免抖动 |
| **日志复制** | 标准 AppendEntries,leader 本地 append → 复制 → 多数派持久化后推进 `commit_index`;**强制 Log Matching** | node_id/在线表/配置的强一致载体 |
| **提交安全(Figure 8)** | 内置:leader 只直接提交**本任期**日志,旧任期日志间接提交 | 换主时已提交的 node_id 不丢——**自研最易写错处,白嫖** |
| **持久化** | `state_mgr` 存 `srv_state`(term/voted_for,投票/append 前 fsync);`log_store` 存日志 | 崩溃恢复基础(G5) |
| **快照 + 压缩** | state_machine 出快照,NuRaft 截断日志,InstallSnapshot 追平落后 follower;v3 **async** 不阻塞 | 日志不无限增长;落后节点快速追平 |
| **成员变更** | 单节点增删(安全方式),变更本身是日志 entry | Center 扩缩容(备用,3 节点固定时不常用) |
| **读一致性** | leader lease read(线性一致);或任意 follower 读(最终一致) | 路由查询/Admin 按需选 |
| **CAP 取向** | **CP**:少数派无 quorum → 不能选主、不能提交 → 写阻塞 | **防脑裂的根本**,见 14.2 #1 |
| **吞吐** | async/streaming 复制(v3 pipeline);每写一次共识 + fsync,通常万级 writes/s | Center 写罕见,远未触及 |
| **线程** | ASIO 线程池(非 M:N) | 与 libev 并存不抢调度(G8) |

### 14.2 业务需求逐条匹配

| # | Center 需求 | 一致性要求 | NuRaft 机制 | 满足 |
|---|---|---|---|---|
| 1 | **node_id 全局唯一分配** | 线性一致,零碰撞 | log entry + apply 全副本同值;**CP 下少数派不可写**,分区两边不会各自发号 | ✅ **完全满足,且强于现状**(替掉 mod 255 近似) |
| 2 | **node_id 槽位复用**(128/256 稀缺) | 强一致 free-list | state_machine 维护空闲槽位集合,分配/回收都是 log entry | ⚠️ **机制满足,策略要自己写**(见 14.3) |
| 3 | 在线表一致 | 偏强一致 | 注册/摘除是 log entry,全副本 apply | ✅ |
| 4 | 配置版本单调 | 强一致 + 版本 | **`log_index` 天然单调递增 = 全局版本号**,免费 | ✅ |
| 5 | 单写者(仅 leader 发号) | 选主 | NuRaft 选举 + 非 leader 重定向(§8.3) | ✅ |
| 6 | **分区不脑裂** | CP | 少数派无 quorum,物理上无法提交 node_id | ✅ **关键,正是 gossip 给不了的** |
| 7 | 崩溃恢复 | 持久化 | log_store + snapshot + srv_state 落盘,重启回放追平 | ✅ |
| 8 | 写吞吐 | 低(注册罕见) | 万级 writes/s;心跳还不走共识 | ✅ 远未触及 |
| 9 | 3 节点容 1 故障 | quorum=2 | 标准 3 副本 | ✅ |
| 10 | 路由查询/Admin 读 | 最终/线性可选 | leader lease read 或本地读 | ✅ |

### 14.3 唯一需要自己设计的点:node_id 槽位复用

- **现状**: `m_uiNextNodeIdAlloc` 游标 + `MergeNodeIdAllocRing`(mod 255 环形),本质是"只增回绕",**不真正回收**崩溃节点的槽位。
- **NuRaft 下**: NuRaft 只提供"把分配动作强一致复制"的机制,**分配策略(哪个槽位空闲)要在 state_machine 里实现**:
  - 状态机持有 `[1,255]` 的**空闲槽位集合 / bitset**;
  - `AllocNodeId` entry apply → 从 free-set 取一个,标记占用;
  - 节点断连(心跳超时)→ Leader 发 `DeregisterNode` entry → apply 把槽位**还回 free-set**,实现真复用(优于现状)。
- **判断**: 这是**业务语义设计**,不是 Raft 难点。NuRaft 把最难的"强一致复制 + 防碰撞"包了,你只需在已强一致的状态机里管一个 255 位的空闲集合——简单且正确性有共识兜底。

### 14.4 结论:满足,且在关键维度升级

> **NuRaft 的 Raft 机制完全覆盖 Center 的业务需求**,且在最薄弱的 **node_id 强一致(#1/#6)** 上从"业务层近似"升级为"真共识 + CP 防脑裂"。唯一要自己写的是 **node_id 槽位复用策略**(14.3)——属业务语义,不是共识难点,且有状态机强一致兜底。Center 用不到的 NuRaft 能力(Multi-Raft、Learner、高吞吐 streaming)不构成负担(不用即不付代价)。**原理层面无阻塞项,方案成立。**

---

## 15. Raft 原理图解 + 日志格式(详解)

> 不假设读者熟悉 Raft,从零讲清楚:选主 → 日志复制 → 提交,以及日志到字节级的格式。

### 15.1 Raft 就三件事:选主 → 日志复制 → 提交

**选主**(保证同一时刻只有一个 Leader 能发号,否则两节点同时发 node_id 会撞):

```
正常(C1 Leader):
   ┌─────────┐  心跳   ┌─────────┐
   │ Leader  │────────►│Follower │ C2
   │  C1     │────┐    └─────────┘
   └─────────┘    └───►┌─────────┐
                       │Follower │ C3
                       └─────────┘

C1 挂了:
   C2/C3 选举超时没收到心跳 → 变 Candidate, term+1, 自投一票, 拉票
     → 拿到多数票(2/3) → 新 Leader
   pre-vote: 拉票前先试探能否赢, 防分区节点瞎涨 term
```

**日志复制 + 提交**(一条 node_id 分配的完整旅程):

```
业务节点: "注册, 给我 node_id"
   │  打到某 Center; 若非 Leader → 重定向到 Leader 重试
   ▼
┌──────────── Leader C1 ────────────┐
│ ① 挑空闲槽位: node_id = 7           │
│ ② 本地日志末尾追加(未提交):         │
│    log: ...#4, #5{term=3,Alloc(7)} │
└────────┬───────────────────────────┘
         │ ③ AppendEntries 发 #5
    ┌────┴────┐
    ▼         ▼
 Follower C2  Follower C3
 追加 #5       追加 #5
 fsync 落盘    fsync 落盘     ← 先落盘
 回 ack ──┐   回 ack ──┐
          ▼            ▼
┌──────────── Leader C1 ────────────┐
│ ④ 多数派 ack(自己+C2=2/3) → commit │
│ ⑤ apply: node_id=7 写进在线表      │
│ ⑥ 回业务节点: node_id = 7          │
└────────────────────────────────────┘
   C2/C3 也各自 apply #5 → 三节点在线表一致
```

要点:
- **未提交 vs 提交**: 日志先追加(未提交),**多数派都落盘**才提交,提交后才 apply(生效)。
- **不脑裂**: 分区时少数派凑不齐多数派 ack,永远提交不了 → 分区两边不会各自发出同一 node_id。
- **崩溃恢复**: #5 已落盘,节点重启读回日志继续。

### 15.2 日志格式(到字节级)

**第 1 层 — NuRaft 的 log_entry(框架字段,它填)**:
```
 ┌─────────┬─────────┬──────────┬───────────────────────────┐
 │ index   │  term   │  type    │  data (变长二进制 buffer)   │
 │ 8B 第几条│ 8B 任期 │ 1B 普通/配置│  ← 装我们的业务 payload     │
 └─────────┴─────────┴──────────┴───────────────────────────┘
   └────── NuRaft 管 ──────┘        └─── 我们管 ───┘
```

**第 2 层 — data 里装我们的业务 entry(protobuf,与 Thunder 一致,放 `coor.proto`)**:
```protobuf
message CenterLogEntry {
  EntryType type       = 1;  // ALLOC_NODE_ID / REGISTER / DEREGISTER / SET_CONFIG
  uint64    request_id = 2;  // 关联挂起的注册协程; apply 后用它唤醒回包
  oneof body {
    AllocNodeIdEntry alloc      = 3;  // { node_identify, 预定的 node_id }
    RegisterEntry    register_  = 4;  // { node_info }
    DeregisterEntry  dereg      = 5;  // { node_id }
    SetConfigEntry   set_config = 6;  // { path, content, version }
  }
}
```
apply: `Decode(data)` → 按 `type` 分派(写在线表 / 还槽位 / 改配置)→ 用 `request_id` 唤醒挂起协程。

**第 3 层 — 落盘(log_store segment 文件)**:
```
deploy/Center/data/raft/log/0000001.seg  (append-only):
 ┌──────┬──────┬───────────────┬──────┬──────┬──────────────┬──
 │len(4)│crc(4)│ entry 序列化   │len(4)│crc(4)│ entry 序列化  │...
 └──────┴──────┴───────────────┴──────┴──────┴──────────────┴──
 - 只追加; 写完 fsync 才回 ack(不丢)
 - 配 index 文件: log_index → 文件偏移
 - 攒够 N 条 / 定时 → 出快照, 删快照前的 seg(压缩)
```

> **完整状态 = 最近快照 + 快照之后的 seg 日志**;恢复/分析需二者结合。各节点 data 目录独立(`conf/conf2/conf3` 各一份)。

### 15.3 谁分配 node_id(角色分工)

> 一句话:**Center 自己分配(业务节点不能自己造),且只有 Leader(主)能造,Follower 不造;主挑值后还需过半 Center 确认才生效。**

```
业务节点 ──要──► Center 集群 ──只有主能造──► Leader 发号
 (不能自己造)                    (Follower 不发, 只指路)

业务节点: "我要注册, 给我 node_id"
   │  打到某个 Center
   ▼
┌─────────── 是 Leader 吗? ───────────┐
│  ├─ 不是 → 回 "Leader 是 C1, 去找它"  │  ← Follower 不分配, 只重定向
│  └─ 是 ──► 分配流程                   │
└─────────────────┬─────────────────────┘
                  ▼
        ┌──────── Leader C1 ────────┐
        │ ① 挑空闲槽位 node_id = 7    │  ← 只有 Leader 挑值
        │ ② 写日志 Alloc(7) 复制      │
        │ ③ 过半 Center 确认(提交)    │  ← 还需多数派背书才生效
        │ ④ apply, 7 正式生效         │
        │ ⑤ 回业务节点 node_id = 7    │
        └──────────────────────────────┘
         C2/C3 只 apply 同一个 7, 不自己挑
```

| 角色 | 在 node_id 分配里干什么 |
|------|------------------------|
| **业务节点**(Interface/Logic/Hello) | **要**号,不分配。自己不能造,必须问 Center |
| **Leader(1 个 Center)** | **挑值 + 发起**。唯一能决定"下一个发 7"的节点 |
| **Follower(其余 Center)** | **不挑值,只背书 + 跟随**。收到注册请求只重定向给 Leader;Leader 发起的日志,它们 ack + apply 同一个值 |

**为什么必须主独占**: 两个节点都能分配 → 可能同时把 7 发给不同业务节点 → node_id 碰撞 → 生成碰撞的 64 位唯一 ID(已落库无法修复)。Raft 保证"同一时刻只有一个 Leader" = "只有一个发号者";且主挑的值要过半确认才生效,分区时少数派的"假主"凑不齐多数、发不出号,杜绝脑裂下的双重分配。

> 与现状对比: 现状 Leader 本地游标 `AllocNextNodeId()` 自增、瞬时、自己说了算;NuRaft 下 Leader 挑值但**要过共识**,换来分区零碰撞。

---

## 附录: 关键代码索引(改动锚点)

- node_id 分配点: `code/Center/src/SessionOnlineNodes.cpp:128`(`AllocNextNodeId`)
- Leader 判定: `code/Center/src/SessionOnlineNodes.cpp:340-341, 348, 357`
- 注册同步流(待改异步): `code/Center/src/CmdNodeRegister/CmdNodeRegister.cpp:61`
- 上报: `code/Center/src/CmdNodeReport/CmdNodeReport.cpp`
- Raft 状态机/选主(待封装/替换): `code/Center/src/SessionRaftCluster.{hpp,cpp}`
- Raft 消息插件(待删): `code/Center/src/CmdRaftAppendEntries/`, `code/Center/src/CmdRaftRequestVote/`
- 配置下发: `code/Center/src/ModuleAdmin/StepSetConfig.cpp`
- 跨线程投递(复用): `code/Net/src/labor/Labor.cpp:660-727`(`PostToEventLoop` / `ev_async`)
- 异步协程范式(复用): `code/Net/include/coro/ThreadPoolAwaitable.hpp`(`RunOnThreadPool`)
- 客户端侧抽象(不动): `code/Net/include/labor/CenterConnector.hpp`
- 库链接参考(动态链): `cmake/ThunderCommon.cmake:146`(`ev` → 同列加 `nuraft`)
- 选型依据: `docs/architecture/center_raft_implementation_options.md`
- 被替代的自研设计: `docs/architecture/raft_leader_lease_design.md`(NuRaft 接管后作废)
