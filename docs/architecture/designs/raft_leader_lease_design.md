# Center Raft 修复设计：node_id 发号的 commit 正确性 + 选举随机化播种

> 2026-06-02 | 基于 `SessionRaftCluster` / `SessionOnlineNodes` 现状审查
> 关联审查结论：CRITICAL（脑裂期 node_id 双分配）+ HIGH（选举随机种子未初始化）
>
> ⚠️ 本版订正了上一版「以 Leader 租约退位为主修复」的框架——那是误用。
> **Raft 的写安全来自 commit 规则（多数派复制后才 apply），不是来自 Leader 退位。**
> 本版以「发号 commit-then-apply」为主修复，租约退位降为可选健壮性增强。
>
> 范围：仅 Center 间 Raft 控制面，不动业务节点 / 路由 shm 下发链路。

---

## 一、先纠正一个认知：写安全靠 commit，不靠退位

### 1.1 canonical Raft 里少数派 Leader 是无害的

```
被隔离到少数派的 Leader：
  · 继续自认 Leader、继续发 AppendEntries（都失败）
  · 关键：提交不了任何新日志（commit 要求复制到多数派）
  · 提交不了 → apply 不了 → 对已提交状态零伤害
  · 分区恢复后撞到更高 term 才退位
```

所以「失去多数派就退位」**不是 Raft 的写安全来源**（它对应 etcd-raft 的 `CheckQuorum` 扩展，作用是保护 lease 读、减少换主扰动，是叠加在 commit 规则之上的优化）。

### 1.2 本项目真正的 bug：apply 先于 commit

```
SessionOnlineNodes.cpp:128
    oNodeInfoObj.set_node_id(raft->AllocNextNodeId());   // 本地立即发号
SessionRaftCluster.cpp:245
    AllocNextNodeId()  // 只 cursor++ 绕回，无任何多数派确认

→ 「简化版 Raft」砍掉了日志复制，于是发号这条"写命令"没经 commit 就 apply 了
→ 少数派 Leader 也能发号 → 与多数派新 Leader 双分配 → snowflake 唯一 ID 碰撞（不可修复）
```

**结论：修复方向不是"让 Leader 及时退位"，而是"把发号变成 committed-then-apply"。**

---

## 二、当前实现 vs Raft 的差距

```
                    canonical Raft          当前 Center 简化版
  日志              有 log + index          ❌ 无，AppendEntries 是空条目
  复制              entries 复制到多数派     ❌ 只复制"全量在线表快照"
  commit            多数派 ack → commitIndex ❌ 无 commitIndex 概念
  apply             commit 后才 apply        ❌ 发号本地立即 apply
  选举              term+votedFor+majority   ✅ 正确
  心跳/租约         AppendEntries 心跳       ✅ 有（Follower 选举租约）
  持久化            term/votedFor/log 落盘   ❌ 全内存（重启归零）
```

✅ 的部分（选举）不动；❌ 里**与 node_id 安全相关的（日志/复制/commit/apply）**是本次要补的。

---

## 三、按 Raft 的目标设计：发号 commit-then-apply

### 3.1 该 commit 的是「绑定」，不是「游标」

真正需要强一致的不变式：

```
任一时刻，不存在两个在线节点共用同一个 node_id
```

只 commit 一个 `next_node_id_alloc` 游标**不够**——新 Leader 仍可能把同一 id 发给不同节点。
**要 commit 的是 `node_identify → node_id` 绑定**，即把「注册」当作一条状态机命令走日志。

### 3.2 Raft 正确的注册时序

```
  注册节点 X            Leader                  Follower多数派
     │  NodeRegister(X)    │                         │
     ├────────────────────>│                         │
     │                     │ ① 选空闲 id, 形成日志条目  │
     │                     │   {register, X, id=k}     │
     │                     │ ② AppendEntries(entry)    │
     │                     ├──────────────────────────>│
     │                     │        ack                │ 复制
     │                     │<──────────────────────────┤
     │                     │ ③ 多数派 ack → commit      │
     │                     │ ④ apply: 绑定 X↔k         │
     │   NodeRegisterRsp(k)│                           │
     │<────────────────────┤  ← 这时才回包             │
```

对照现状（同步链 `AddNode→AllocNextNodeId→SendToClient` 当场回包）：**回包要推迟到 commit 之后**。

---

## 四、要正面解决的架构问题

### P1 — 没有"日志"，得补回最小复制日志
当前 `AppendEntries` 空条目。要 commit-then-apply，必须承载 register 事件的复制日志（至少 index + term + {register/dealloc, identify, id}），并实现日志匹配与 commitIndex 推进。**这是被简化掉的核心，补回来是重构，不是补丁。**

### P2 — 注册从「同步应答」变「异步等 commit」
```
现状: 收到注册 ──同步──> 发号 ──> 立即回包
Raft: 收到注册 ──> 追加日志 ──> 复制 ──> 等多数派 ack ──> commit ──> apply ──> 回包
                                                                    ▲
                                              请求需挂起(pending)到此处
```
`CmdNodeRegister::AnyMessage` 的同步 `SendToClient` 模型要改成 pending + commit 回调。（或用预提交区块规避延迟，见 §六）

### P3 — commit 单位是绑定 → 范围推到「成员变更日志」
见 §3.1。这点决定了 P1 躲不掉：要 commit 的不是一个数，是一系列注册/下线事件。

### P4 — 快照与日志要对齐（snapshot = 日志压缩）
现状在线表是 leader 全量 wholesale 替换（`ApplyOnlineSnapshotFromLeader`）。注册走 log 后，快照必须变成带 `lastIncludedIndex/Term` 的**日志快照**，否则「全量快照」与「日志复制」两套机制互相打架，follower 不知信哪个。

```
日志:     [.. idx100 idx101 idx102 ..]
快照:     [<= idx99 已压缩]  lastIncludedIndex=99
          ↑ follower 落后太多时用快照追，其余用日志增量
```

---

## 五、独立隐患（与分区无关，建议先单独止血）

### P5 — `AllocNextNodeId` 盲目 `cursor++ wrap`，无 liveness 检查 ⚠️

```
SessionRaftCluster.cpp:245
    id = m_uiNextNodeIdAlloc; ++m_uiNextNodeIdAlloc;   // 到 256 绕回 1，不看是否在用

id 空间仅 128(MS) / 256：长跑集群节点反复上下线 → 游标绕回 →
把一个"仍在线节点持有的 id"再发出去 → 单 Leader、无分区也碰撞
```

- 比脑裂场景**更常见**（现网就可能踩）。
- 正确做法：发号时**从"当前在线集合"里选空闲 id**，而非 cursor++。
- **独立于整个 Raft 重构，改动小，建议最先修。**

```
旧:  next free id = cursor++ (wrap)
新:  for id in [1..NODE_ID_MAX-1] 从 cursor 起环扫:
         if id 不在 m_mapOnlineNodes 的已用集合: 取之; break
     若一圈都满 → 返回 0 (集群 id 耗尽, 报错)
```

---

## 六、取舍项（待定）

### P6 — `MergeNodeIdAllocRing` 启发式合并将淘汰
mod-255「谁先谁后」的 ring 合并（cpp:47）是 best-effort 产物。真 Raft 下 follower 采纳 committed 值，不需要猜 → 改 B 后删除，否则与 commit 语义冲突。

### P7 — 持久化：node_id 不敏感，但选举状态敏感（分开决定）
```
全集群重启 → 业务节点重注册领新 id → 旧已生成唯一 ID 时间戳不同, 不冲突
  → 单为 node_id 不必持久化日志
但 term/votedFor 不持久化 → 同 term 二次投票 → 理论上可选出两个 Leader
  → 选举状态该不该落盘要单独决定, 别一刀切
```

### P8 — 若保留 CheckQuorum/租约退位做附加健壮性，必须配 PreVote
租约退位现在**不是安全依赖**（commit 规则才是），可做可不做。若做（减少分区恢复换主扰动 / 为未来 lease 读铺路），**必须配 PreVote**，否则被隔离节点 term 暴涨、恢复后扰动现任 Leader。

### P_区块 — 每次注册跑一轮 commit 延迟高 → 预提交区块
```
Leader 平时预先 commit 一段 id 区块(majority ack) → 注册时从已 committed 区块本地发号(同步快)
  区块耗尽再补
约束: 区块大小 ≪ 在线节点余量; 区块内仍要按 P5 跳过在线 id
```
这是「完整最小日志」与「现状」之间的中间档：规避 P2 延迟、改动小些，但细节（区块 + 绕回 + liveness 协调）要小心。

---

## 七、选举随机化独立播种（F2，与上面正交，仍需修）

### 问题
```
进程启动 → 未调用 srand()(GetPassword 在选举路径外) → std::rand() 默认 seed=1
  → 所有 Center 进程随机序列完全相同
  → follower_deadline / candidate_retry 高度相关 → split vote → 收敛慢
```

### 方案：自播种 `RandUnit()` 替换全部 `std::rand()`
```
RandUnit() 返回 [0,1):
  thread_local std::mt19937 seed_seq{ pid, steady_clock ns, random_device }
                                       ↑同秒启动也不同  ↑高精度  ↑系统熵
```
替换 5 处（行为分布不变，只是真随机）：`cpp:39 / 238 / 462 / 559 / 605`
```
(static_cast<ev_tstamp>(std::rand() % 1000) / 1000.0)  ──>  RandUnit()
```

---

## 八、两条路线对比 + 决策点

```
                  ┌────────────────────┬──────────────────────┬─────────────┐
                  │ 路线①最小复制日志    │ 路线②预提交区块         │ 现状(基线)   │
  ────────────────┼────────────────────┼──────────────────────┼─────────────┤
  Raft 正确性      │ ✅ 完整 commit 规则  │ ⚠️ 近似(区块已 commit)  │ ❌ 无       │
  node_id 碰撞     │ 数学杜绝            │ 数学杜绝(区块内安全)     │ 会碰撞      │
  注册延迟         │ 每次一轮复制(高)     │ 区块内本地发号(低)       │ 本地(最低)  │
  改动量           │ 大(P1-P4 全做)      │ 中(P2 规避, 仍需复制区块) │ -          │
  快照对齐 P4      │ 必做                │ 必做(区块也是 committed)  │ -          │
  ────────────────┴────────────────────┴──────────────────────┴─────────────┘
```

> P5（绕回 liveness）两条路线都要修，且独立可先修。

### 待你决策
1. **重构路线**：①最小复制日志（最干净最 Raft，改动大）还是 ②预提交区块（中间档，规避延迟，细节要小心）？
2. **P5 是否先单独止血**：它独立于 Raft 重构、现网就可能踩、改动小。建议先修 P5，再规划路线 ①/②。
3. **P7 选举状态持久化** / **P8 CheckQuorum+PreVote**：纳入本期还是后续？

---

## 九、验证计划

```
1. 编译         ./deploy.sh build                        零告警 (-Wall -Wextra)
2. 选举单测     ctest -R CenterRaft --output-on-failure   现有用例全过
3. P5 用例      构造 >128 次注册使游标绕回, 断言不会把在线 id 重发
4. commit 用例  3~5 Center; 隔离 Leader →
                断言少数派 Leader 发不出号(注册返回 err, 无新绑定)
                恢复后单 Leader 收敛, 绑定一致
5. F2 种子用例  同秒拉起 3 Center; 打印各自首个 deadline → 互不相同
6. E2E          ./deploy.sh test e2e --skip-build         注册/路由链路未破坏
```

---

## 十、关键代码索引

- 发号(本地立即 apply): `SessionOnlineNodes.cpp:128`
- 发号游标(cursor++ wrap, 无 liveness): `SessionRaftCluster.cpp:245 AllocNextNodeId`
- 注册同步回包: `CmdNodeRegister.cpp:77-98 AnyMessage`
- AppendEntries(空条目): `SessionRaftCluster.cpp:484 RaftSendAppendEntriesToAll`
- 全量快照替换: `SessionOnlineNodes.cpp:416 ApplyOnlineSnapshotFromLeader`
- ring 合并(将淘汰): `SessionRaftCluster.cpp:47 MergeNodeIdAllocRing`
- 退位唯一路径: `SessionRaftCluster.cpp:672 OnRaftAppendEntriesResponse`
- std::rand 5 处: `SessionRaftCluster.cpp:39 / 238 / 462 / 559 / 605`
- srand 唯一调用: `code/Util/src/util/CommonUtils.hpp:167 GetPassword`
```
