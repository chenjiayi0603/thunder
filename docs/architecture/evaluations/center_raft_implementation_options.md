# Center 共识层实现选型: 手写 vs 嵌库 vs 外部 etcd

> 日期: 2026-06-02
> 状态: 评估 / 技术选型
> 驱动问题: 若 Center 要保留强一致共识,实现路径怎么选——手写完善自研 Raft、嵌入成熟 C++ Raft 库、还是用外部 etcd？
> 结论先行: **自研 Raft 的难点不在算法,在"证明它在所有故障组合下不出错"——这部分工作量以年计且大头是验证设施,连 eBay 维护的 NuRaft 在 2026 年都还在抓数据竞争/段错误。若目标是生产正确性,优先「嵌 NuRaft」:它 T1–T4 全覆盖、不抢 Thunder 的 libev/协程运行时、复用已有 standalone Asio、shm 零跳完整保留。手写仅在「学习/作品集」目标下才划算。**

> 关联评估: [etcd 替代方案](./etcd_as_center_evaluation.md) · [gossip 去中心化方案](./gossip_decentralization_evaluation.md) · [Center vs Nacos](./center_vs_nacos_evaluation.md) · [leader lease 设计](./raft_leader_lease_design.md)

---

## 1. 自研 Raft 的难度分层(T1–T4)

"参照 etcd 完善自研 Raft"不是线性"完善",是分层陡增的质变。当前 Center 是「选主 + 心跳租约 + 在线表快照随心跳同步,**不做完整日志复制**」,处在 T1。

| 层级 | 内容 | 现状 | 难度 |
|------|------|------|------|
| **T1** | 选主 + 心跳 + 状态快照同步 | ✅ 已有 | 低,happy path 几周能跑 |
| **T2** | 完善选举(term/任期、split vote、随机超时)+ lease 读 | 🚧 在做([leader lease 文档](./raft_leader_lease_design.md)) | 中,可达成 |
| **T3** | **真正的日志复制 + 持久化 + 安全性不变式** | ❌ 没有 | 高,大多数自研 Raft 死在这 |
| **T4** | 快照/日志压缩 + 成员变更 + 可验证测试 | ❌ 没有 | 很高,以年计 |

从 T1 跨到 T3/T4 是质变,不是"完善"。

### 1.1 T3/T4 真正的难点(算法书轻描淡写、实战要命)

1. **Figure 8 提交规则**: Leader 只能直接提交**自己任期**的日志,旧任期日志只能间接提交。违反直觉,漏了它已提交日志会在换主时丢失。
2. **持久化 / WAL**: `currentTerm`、`votedFor`、日志必须**先 fsync 落盘再响应**;崩溃恢复、半截写、fsync 语义。
3. **快照 + 日志截断**: 日志不能无限长,InstallSnapshot + 从快照恢复。
4. **成员变更**: joint consensus / 单节点变更,Raft 里出 bug 最多的地方。
5. **验证设施(真正的工作量黑洞)**: 故障注入(分区/丢包/乱序/重复/崩溃重启/时钟漂移)+ 确定性仿真 + TLA+ 模型验证。**没有这套,你的 Raft "看起来能跑"但藏潜伏 bug,会在生产凌晨三点的分区里爆。** 这套测试设施的工作量与实现本身一个量级。

### 1.2 该先自问: 你真需要 T3/T4 吗

Center 数据集**极小**(在线表 + node_id + 一点配置)。完整 Raft 是为"复制任意长操作日志"设计的;你要复制的只是一小坨可变状态。很可能 **T1+T2 已够用**,硬上 T3/T4 是给自己加用不上的负债(违背"简洁优先")。

---

## 2. 三条实现路径

| 路径 | 形态 | 消除自研 Raft 负债 | 新增组件/依赖 | shm 零跳 | 适用目标 |
|------|------|:---:|------|:---:|------|
| **A. 手写完善自研 Raft** | 自己进程内手写 | ❌ 自己背正确性 | 无(但要自建测试设施) | ✅ 保留 | 学习 / 作品集 / 打磨功底 |
| **B. 嵌入 C++ Raft 库** | 自己进程内嵌库 | ✅ 甩给库 | 一个 C++ 库(无外部进程) | ✅ 保留 | **生产正确性 + 不增独立部署** |
| **C. 外部 etcd** | 独立 etcd 集群 | ✅ 甩给 etcd | etcd 集群 + gRPC 客户端 | ✅ 保留 | 已愿运维外部协调集群 |

三条路 shm 那一跳(`Loader → shm → Worker`)都完整保留,差异只在"共识这件难事谁来做"。

- **A** 没去掉负债,只是把简化 Raft 做成完整 Raft——正确性仍是自己的。
- **B** 把共识甩给验证过的库,**且不引入外部进程**(Center 还是自己进程,内核换掉)——比 C 更轻。
- **C** 见 [etcd 评估](./etcd_as_center_evaluation.md),适合愿意运维外部集群、且想顺带解决跨服务通用注册的场景。

---

## 3. C++ Raft 库选型

| 库 | 来源 | 评估 |
|----|------|------|
| **NuRaft** | eBay(cornerstone 衍生) | ✅ **对 Thunder 最适配**(见 §4) |
| braft | 百度,基于 brpc | 最成熟之一,但**绑死 brpc → 拖来 bthread 运行时,与 libev 冲突** |
| raft-rs | TiKV,从 etcd/raft 直接移植 | 血统最正,但 **Rust**,C++ 项目要维护 FFI,排除 |
| logcabin | Raft 作者 Ongaro 本人 C++ | **读源码学原理最佳**,但停止维护、非嵌入式库,生产不选 |

---

## 4. 为什么 NuRaft 最适配 Thunder

**1. 不抢运行时(对 braft 的决定性优势)**
- 传输/线程/状态机/日志存储全可插拔,不强加调度模型,能与 **libev 主线程 + StepCo20 协程**共存。
- braft 绑 brpc,brpc 自带 **bthread**(M:N 协程运行时)→ 进程内同时跑 libev 和 bthread 两套调度,直接冲突。这一条基本否决 braft。

**2. 依赖面几乎零新增**
- NuRaft 默认传输基于 **standalone ASIO**,而 Thunder **已在用**(`AsioUringIoBackend`)。

**3. 数据规模匹配**
- NuRaft 轻量,正合"复制一小坨状态";braft 偏重型 KV(TiKV 级),对此场景杀鸡用牛刀。

**4. 你只写状态机,不碰共识**
- 实现 `state_machine`(apply 日志 → 更新在线表/node_id/配置)+ `log_store` 回调即可。
- T1–T4 全是库给的: `node_id` 强一致发号 = 一条 Raft 日志 apply;在线表 = 状态机;**Figure 8 / WAL / InstallSnapshot / 成员变更全不用碰**。

**5. shm 零跳一点不丢**
- NuRaft 只负责 Center 副本间共识;节点内 `Loader → shm → Worker` 照旧,shm 零跳直推优势完整保留(见 [center_vs_nacos_evaluation.md](./center_vs_nacos_evaluation.md) §3.1)。

**6. 生产背书**
- ClickHouse Keeper(替代 ZooKeeper 的强一致组件)建在 NuRaft 上,经大规模生产检验。

### 4.1 T1–T4 覆盖对照

| 层级 | 手写自研 | NuRaft | braft |
|------|:---:|:---:|:---:|
| T1 选主 + 心跳 + 快照同步 | ✅ 已有 | ✅ | ✅ |
| T2 完善选举 + lease 读 | 🚧 | ✅(priority election、pre-vote) | ✅ |
| T3 日志复制 + 持久化 + 安全不变式 | ❌ 最难 | ✅ 内置 | ✅ |
| T4 快照/压缩 + 成员变更 + 测试 | ❌ 以年计 | ✅(async log compaction、动态成员变更、自带测试) | ✅ |

**嵌 NuRaft = 直接拿到 T1–T4 全部,且经过验证。**

---

## 5. NuRaft 仓库现状(2026-06 联网核实)

**Release**

| 版本 | 日期 | 备注 |
|------|------|------|
| **v3.0.0** | 2025-03-31(最新) | streaming mode、async log compaction、修 asio_rpc_listener 数据竞争 + 整数溢出 |
| v2.1.0 | 2025-01-18 | |
| v2.0.0 | 2024-11-10 | |
| v1.3.0 | 2024-06-30 | |

- 最新正式版 **v3.0.0(2025-03)**,到 2026-06 约 14 个月未发新正式版,**发版节奏慢(走大版本)**。
- ⭐ ~1.2k stars,体量中等,有真实生产用户(ClickHouse Keeper)。

**Issue(73 open)—— 活跃但有并发隐患**
- issue 持续更新到 2026-05,**代码层面仍活跃维护**(非弃坑)。
- 但 2026 年仍开着并发类 bug,值得正视:
  - #654 Reload TLS context(2026-05)
  - #653 段错误: 迭代器失效(2026-05)
  - #646 角色切换时并发快照终结崩溃(2026-03)
  - #644 `handle_commit` 数据竞争(2026-03)

**解读**: 即便 eBay 维护的成熟库,**并发正确性仍是持续战场**——这恰恰佐证"自研只会更难"。区别在于 NuRaft 有团队在持续抓这些 bug,你白嫖这份维护;手写则全得自己踩自己修。

---

## 6. 结论与落地提醒

**结论**:
- **生产正确性** → 嵌 **NuRaft**(T1–T4 全覆盖、不抢运行时、复用 Asio、无外部进程)。
- **愿运维外部集群 / 想顺带通用注册** → 外部 **etcd**(见 etcd 评估)。
- **学习 / 作品集** → 手写参照 etcd/raft 论文 + 读 **logcabin**;但诚实预期: happy path 几周,敢上生产几个月起,一半时间在故障注入测试。

**落地提醒(基于 §5 现状)**:
- 锁版本: 用 **v3.0.0 release** 或锁定含近期并发修复的 master commit,**别用浮动 master**。
- 集成后**自己也要跑 TSan + 故障注入**(网络分区/崩溃重启)——与 CLAUDE.md"涉及并发改动 TSan 必跑"一致。
- NuRaft 只解决 Center 副本间共识;shm 那层胶水(Watch/apply → memcpy + version++)仍自己写,但那是简单 IPC,不是 Raft。
- 集成隔离在 `CenterConnector` 抽象层(`code/Net/include/labor/CenterConnector.hpp`),保证可回退。

---

## 附录: 关键代码索引

- 控制面抽象: `code/Net/include/labor/CenterConnector.hpp`(嵌库/etcd 实现的落点)
- node_id 生成 / 使用: `code/Util/src/util/CommonUtils.hpp`, `code/Net/include/labor/Labor.hpp`
- 在线表 / 订阅路由: `code/Center/src/SessionOnlineNodes.hpp`
- Raft / leader lease(待替换): `docs/architecture/center_stagement.md`, `docs/architecture/designs/raft_leader_lease_design.md`
- shm 直推优势拆解: `docs/architecture/evaluations/center_vs_nacos_evaluation.md` §3.1
