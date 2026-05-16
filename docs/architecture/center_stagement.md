## Center 选举与一致性说明

### 1) `need_leadership` 的定位
- 业务节点在 leader 未稳定前会对多个 Center 发请求，稳定后优先走 leader。
- 因此 `need_leadership=false` 在本地联调/集成测试中通常可接受，核心影响集中在选举窗口期。
- 主要变化是：选举窗口内 `node_id` 分配从“严格 leader 门禁”变为“先可用、后收敛”（选举时最终一致性分配nodeid，选举后强一致性分配nodeid）。
- 建议：测试环境可设 `false` 以提升稳态；生产建议默认 `true`（强一致性分配nodeid，选举时不分配）。

### 2) 选举模型（当前实现）
当前 `Center` 使用简化版 Raft（偏选主与心跳租约，不做完整日志复制）：
- **成员与多数派**：从 `CenterCmd.json` 的 `centers` 读取（无则回退 `custom.centers`），成员不足会补入本机；多数派为 `N/2 + 1`。
- **角色状态**：`Follower / Candidate / Leader`。
- **核心状态量**：`m_raftTerm`、`m_raftVotedFor`、`m_raftLeaderId`、`m_raftVotesGranted`、`m_raftLastLeaderContact`。

### 3) 时间参数与触发条件
- 冷启动 follower 首次发起选举：`0.20 ~ 0.50s`。
- candidate 选举重试间隔：`0.08 ~ 0.20s`。
- follower 租约：`2 * center_beat + (1.0 ~ 1.5s)`；`center_beat=3s` 时约 `7.0 ~ 7.5s`。
- 体感时间（大致）：
  - 首次选举通常约 `1s` 内可收敛（多数情况下几百毫秒到 1 秒）。
  - 再次选举（leader 失联后的重选）应分两段理解：先等待 follower 租约超时，再进入选举收敛。  
    - 总耗时（大致）≈ `2 * center_beat + (1.0~1.5s) + (0.1~1.0s)`。  
    - 例如 `center_beat=2s` 时约 `5.1~6.5s`，`center_beat=3s` 时约 `7.1~8.5s`。  
    - 其中百毫秒到 1 秒量级主要是“进入选举后的收敛时间”，不是从失联起算的总时长。

对比 Nacos（是否选举与收敛时间）：
- Nacos 集群自身也有一致性与选主机制（并非“完全不选举”），但节点发现/健康判定通常通过服务端心跳与注册表同步完成，业务侧感知的是“实例状态收敛”而非直接感知 leader 选举过程。
- 时间语义上，Center 更强调“leader 门禁 + 选举窗口”；Nacos 更强调“健康检查周期 + 元数据同步窗口”。
- 因此两者都存在短暂不一致窗口：Center 主要受租约与选举影响，Nacos 主要受心跳超时、剔除阈值和注册表传播延迟影响。
    
- `RaftTick()` 逻辑：
  - Leader 仅发心跳；
  - Candidate 到重试点后发起新一轮拉票；
  - Follower 若租约内收到 leader 心跳则不抢选，超时后才进入选举。

### 4) 投票与稳定 leader 判定
- `HandleRaftRequestVote()`：
  - 请求 term 小于本地 term：拒绝；
  - 请求 term 大于本地 term：先降级 follower 再评估；
  - 同 term 且本机仍是 leader：拒绝；
  - 本 term 未投票或已投同 candidate：同意；
  - 达到多数票后 Candidate 晋升 Leader。
- “稳定 leader”判定：非 candidate 且 `leader_id` 非空。
- `NodeRegister/NodeReport` 依此返回是否 `err=2`（`no stable raft leader`）。

### 5) `term` 与 `voter_next_node_id_alloc_hint` 的作用
- `term` 完整参与新旧任期比较、角色转换、收到更高 term 时降级。
- `voter_next_node_id_alloc_hint` 在投票回包/心跳回包阶段用于合并 `node_id` 游标：
  - 主要提升状态收敛效率；
  - 不直接改变“谁当选 leader”的规则（当选仍由 `term + votedFor + majority` 决定）；
  - 可间接降低选主后的抖动与重试成本。

### 6) 实践建议
- **本地测试/联调**：可接受 `need_leadership=false`，优先保障链路可测与快速收敛。
- **生产环境**：建议 `need_leadership=true`，保持 leader 门禁与一致性约束。


# Center 相对 Nacos 的重点结论

## 结论

| 维度 | Center（当前项目） | Nacos（通用平台） | 结论 |
| --- | --- | --- | --- |
| 语义匹配 | 与 `NodeReport/NodeNotice`、路由订阅、Center 运维命令原生一致 | 需做语义映射和兼容层 | 当前项目中 Center 改造成本更低 |
| 控制面链路 | 注册/路由/观测一体，排障路径短 | 治理能力更通用，但需要接入改造 | 短期交付优先 Center |
| 一致性模型 | 与现有 Raft/leader 行为直接对齐 | 需重新对齐选主与状态边界 | 迁移期有灰色故障风险 |
| 性能路径 | 协议短链路，少转换层 | 通用模型开销更高（但生态成熟） | 热路径场景 Center 更可控 |
| 生态能力 | 定制快、与现有代码深耦合 | 跨语言、平台化、治理生态更强 | 长期平台化 Nacos 更有优势 |

> **补充说明**：虽然 Nacos 具备注册、路由、观测等核心功能，并且在平台化、通用场景下能力更全面，但 Nacos 并不支持 nodeid 分配（中心化节点编号分配），而 Center 原生支持该功能，能更好满足当前业务场景。同时，Center 整体链路更短、性能开销更低、内存占用也更少，排障和演进成本相对较低。长期来看，Nacos 的注册/路由/观测能力可作为平台融合与扩展的选项，但当前阶段 Center 在本项目下具备明显优势。

## 重点建议

| 目标 | 推荐策略 |
| --- | --- |
| 短中期稳定交付、最小改造风险 | 保留 `Center` 作为主控制面 |
| 长期平台统一治理 | 采用双层渐进：Nacos 承担注册/配置，Center 逐步瘦身 |
| 变更节奏 | 不建议无兼容层与双栈验证时直接全量替换 `Center -> Nacos` |

