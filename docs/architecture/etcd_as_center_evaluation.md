# etcd 替代 Center 的可行性评估（原语映射 + 保留 shm 层）

> 日期: 2026-06-02
> 状态: 评估 / 技术探索（暂不落地实现）
> 驱动目标: ① 降低自研 Raft 的正确性维护负担；② 评估用成熟强一致 KV 接管控制面、同时保留 shm 零跳直推的可行性
> 结论先行: **etcd 能以原语方式接管 Center 的全部职责，且恰好原生解决 [gossip 方案](./gossip_decentralization_evaluation.md)绕不过去的枢纽障碍——`node_id` 强一致分配（etcd 的 txn/CAS 主场）。代价是引入一个外部强一致组件（仍需部署/运维，但无需自研 Raft）。节点内 `Loader → shm → Worker` 一跳完整保留，「shm 零跳直推」优势不丢。**

---

## 1. 背景与定位

当前 Thunder 用自研 **Center 集群**（简化 Raft：选主 + 心跳租约，不做完整日志复制）做控制面。它的负担有两层:

- **运维层**: 单独部署、有选主窗口（冷启动约 1s 收敛，换主约 `2*center_beat + 1~1.5s`，`center_beat=3s` 时约 7~8.5s）、需排查 Raft 状态。
- **正确性层**(更隐蔽): 自研 Raft / leader lease（见 [raft_leader_lease_design.md](./raft_leader_lease_design.md)）的时钟漂移、租约窗口、脑裂边界等 corner case 是**长期负债**——这些 etcd 已经磨了十年。

[gossip 评估](./gossip_decentralization_evaluation.md) 想用「去中心化 + 自收敛」消除运维层负担，但卡在 `node_id` 强一致(AP 与强一致本质冲突)。

**etcd 走的是相反方向**: 不去中心化，而是把"强一致中心"这件事**外包给成熟组件**——自己不再写 Raft，但保留一个(更可靠的)协调点。它和 gossip 是两条互补路线，本篇与 gossip 篇平行，并在 §5 做 A/B 对照。

etcd 不是开箱即用的注册/配置中心，而是**强一致 KV + watch + lease + txn 的原语层**(k8s 的服务发现/ConfigMap 即建于其上)。Thunder 要做的是把现有 Center 逻辑**重写为 etcd 原语的薄客户端**，而非接入一个成品。

---

## 2. Center 职责盘点 × etcd 适配度

沿用 gossip 篇的职责清单，换列评估 etcd:

| # | 职责 | 一致性要求 | gossip 适配度 | **etcd 适配度** |
|---|------|-----------|--------------|----------------|
| 1 | 注册 / 发现 / 健康检测（35s 心跳摘除） | 最终一致即可 | ✅ 强 | ✅ Lease + KeepAlive + Watch |
| 2 | **node_id 中心化分配（强一致唯一发号）** | **强一致** | ❌ 本质冲突 | ✅ **Txn/CAS，主场**（见 §3） |
| 3 | 订阅式路由分发（按 `node_type` 推 `NodeNotice`） | 最终一致即可 | ✅ 强 | ✅ prefix Watch |
| 4 | 配置分发（源 → Loader → shm → Worker） | 偏强一致（带版本号） | ⚠️ 需版本/反熵 | ✅ revision 天然版本 |
| 5 | Raft 选主 + 在线表快照同步 | 强一致 | ➖ 去掉即目标 | ✅ 外包给 etcd 自身 Raft |
| 6 | Admin 运维页（全局视图） | 读最终一致 | ⚠️ 仅最终一致视图 | ✅ 线性一致读，全局强一致视图 |

**与 gossip 的关键差异在 #2 和 #6**: gossip 的两个"❌/⚠️"在 etcd 这里都是"✅"。etcd 没有 gossip 那个枢纽障碍。

---

## 3. 核心: node_id 强一致分配 —— etcd 的主场

gossip 篇 §3/§4 论证了 `node_id` 是枢纽:它被编进每个 64bit snowflake 唯一 ID(默认 7bit = **128 槽位**)，必须**全局唯一**且**碰撞不可事后修复**(多为业务主键)。这是一个**全局互斥的稀缺资源分配**问题，需要强一致。

gossip 是 AP，分区下两分区可能选中同一槽位 → 碰撞窗口不可消除。**而 etcd 是 CP，这正是它存在的理由。**

### 3.1 用 etcd txn 原子分配 node_id

两种实现，按需选:

**方式 A — 自增计数器(适合只增不复用)**
```
# 伪代码: 原子自增一个全局计数器 key
txn:
  compare:   value("/thunder/node_id_seq") == <旧值 v>
  success:   put("/thunder/node_id_seq", v+1)
  failure:   重读重试
分到的 node_id = v   # CAS 保证两个节点不会拿到同一个 v
```

**方式 B — 槽位占位(适合 128 槽位需复用)**
```
# 抢占一个空闲槽位 i，带租约，下线自动释放
txn:
  compare:   create_revision("/thunder/node_slot/<i>") == 0   # 槽位为空
  success:   put("/thunder/node_slot/<i>", node_meta, lease=L)
  failure:   换下一个 i 重试
# 节点存活靠 KeepAlive 续租；崩溃 → 租约过期 → 槽位自动回收复用
```

- **强一致保证**: etcd 的 txn 是线性一致的，**两个节点对同一槽位的占位只有一个成功**。分区少数派根本无法写入(无 quorum)，从物理上杜绝了 gossip 那种"双分区各自发号"。
- **槽位复用**: 方式 B 把 node_id 绑在租约上，节点崩溃后租约过期、槽位自动回收 —— 解决了 128 槽位稀缺 + 弹性伸缩的矛盾，这是 gossip 方案 D(静态预分配)做不到的。

> **结论**: gossip 评估里"只有静态预分配(放弃弹性)或换 ID 方案(大工程)才能去掉组件"的两难，在 etcd 下**不存在**——既保留 snowflake、又保留运行时弹性分配、又强一致零碰撞。这是 etcd 相对 gossip 最大的价值点。

---

## 4. 其余职责的 etcd 映射

### 4.1 注册 / 发现 / 健康检测（#1）—— Lease + KeepAlive + Watch
- 注册 = `put("/thunder/nodes/<type>/<node_id>", meta, lease=L)`；KeepAlive 续租 = 心跳；租约过期 = 自动摘除(替代 35s 心跳超时)。
- 健康检测精度由租约 TTL 决定，可比现状更灵敏，且无 leader 单点。
- 发现 = `Range("/thunder/nodes/<type>/")` 拉一把。

### 4.2 路由分发（#3）—— prefix Watch
- 各节点 `Watch("/thunder/nodes/")`，成员变更事件实时推送(etcd Watch 是事件流，非轮询)，收敛延迟通常优于 gossip 的 O(log N) 轮次传播。
- **下游链路原样保留**: 拿到变更后，仍走现有 `version++ 原子递增 → 先写 blob 再写 len → shm` 写入 Worker。上游来源从"自研 Center 推送"换成"etcd Watch 回调"，shm 那一跳不动。

### 4.3 配置分发（#4）—— revision 即天然版本号
- 配置存为 etcd key，etcd 每次写自带**全局单调递增 revision**，天然满足"要么旧要么新、单调"——gossip 篇里要额外设计的"版本号 + 反熵"，etcd 免费给。
- Watch 配置 key → 取到新值 + revision → 写入配置 shm(沿用现有半包保护)。

### 4.4 Admin 视图（#6）—— 线性一致读
- etcd 支持 linearizable read，Admin 可拿到**全局强一致视图**，无需像 gossip 那样降级为"最终一致视图"、改运维习惯。

---

## 5. A/B 对照: etcd vs gossip

两条路线的本质区别: **gossip 消灭中心(去中心化)，etcd 替换中心(换更可靠的中心)。**

| 维度 | Gossip 方案 | etcd 方案 |
|------|------------|-----------|
| 控制面形态 | 对等 / 自收敛，无 leader | 仍有强一致中心，但外包给成熟件 |
| 是否真正"去掉组件" | ⚠️ 只有方案 D/B 才彻底，否则仍留协调点 | ❌ 没去掉(仍需部署 etcd)，但**不再自研 Raft** |
| **node_id 强一致(#2)** | ❌ **枢纽障碍，无解** | ✅ **txn/CAS 主场，原生解决** |
| 弹性伸缩 + 槽位复用 | gossip 抢占有碰撞窗口；静态预分配放弃弹性 | ✅ 租约占位，崩溃自动回收，弹性+强一致兼得 |
| 路由收敛延迟 | O(log N) 轮次，亚秒~秒级 | Watch 事件流，通常更快 |
| 配置一致性 | 需自行补版本 + 反熵 | revision 天然版本，免费 |
| Admin 视图 | 降级为最终一致 | 线性一致，全局视图 |
| 运维负担 | 少了 Center，多了 gossip 运行时(O(N) 周期流量需压测) | 少了自研 Raft，多了 etcd 集群(3 节点) |
| 正确性负债 | 自研 gossip/冲突处理仍要自己保证 | **甩给 etcd**(磨了十年) |
| 新增依赖 | C++ gossip 库(memberlist 思路移植或集成) | etcd 集群 + etcd C++ 客户端(gRPC) |
| 内存/形态 | 嵌在节点内，无独立进程 | etcd 单二进制(Go，几十 MB 级)，无 JVM |
| 适用前提 | 能容忍最终一致 + 解决 node_id | 接受保留一个(更可靠的)外部协调点 |

**一句话**: 若目标是"彻底去中心化"，gossip 是方向但被 node_id 钉死；若目标是"去掉自研 Raft 的正确性负债、同时不破坏 shm 零跳"，**etcd 是更稳妥的落地路径，且恰好解了 gossip 的死结**。两者甚至可组合: etcd 只管 #2 强一致发号，gossip 管 #1/#3 成员路由(见 §8)。

---

## 6. 架构: etcd 替谁，shm 保留谁

```
现状:   主 Center(自研Raft) ──推送──> Loader ──> shm ──轮询──> Worker
                  │ 选主/注册/发号/配置源/路由
                  └─ 自研 Raft + leader lease(正确性负债)

换后:   etcd 集群(成熟Raft) ──Watch/Lease/Txn──> 节点内 EtcdConnector ──> Loader ──> shm ──轮询──> Worker
                  │ 注册(lease)/发号(txn)/配置源(revision)/路由(watch)
                  └─ 正确性甩给 etcd
        ▲ 替掉自研 Raft+KV+跨机强一致              ▲ 这一跳完整保留
```

- etcd **替掉**: 自研 Raft 选主、KV 存储、跨节点强一致(发号/注册/配置源/在线表)。
- **保留不动**: `Loader → shm → Worker` 这一跳。前述"shm 零跳直推 Worker"(节点内读配置纳秒级、多进程零拷贝共享、Center/etcd 宕机时 Worker 读旧值续跑)优势**一点不丢**——etcd 只管到节点级，节点内 Worker 仍读本地 shm。
- Thunder 需新写的只是**"把 etcd 数据搬进 shm"这层薄胶水**(Watch 回调 → 写 shm)，而非维护 Raft。

详见 [center_vs_nacos_evaluation.md](./center_vs_nacos_evaluation.md) §3.1 对 shm 直推优势的拆解。

---

## 7. 工程现状的有利/不利因素

**有利**:
- 已有 `CenterConnector` 策略接口(`code/Net/include/labor/CenterConnector.hpp`)，Manager 经它与控制面通信，理论上可新增 `EtcdCenterConnector` 实现。
- 控制面来源与 shm 下发链路已解耦，shm 那一跳可原样复用。
- etcd 语义与 `CenterConnector` 现有方法**契合度高于 gossip**: `ReportNodeStatus(is_register)` → put/delete + lease；`Registered` 携带 `node_id` → txn 分配结果；仍有"中心"概念，无需像 gossip 那样把接口推倒(gossip 没有"上报给谁/哪条是 Center 连接"的概念)。

**不利**:
- **引入外部 etcd 集群**(生产 3 节点)是新部署面 —— 与"降低运维复杂度"目标要权衡: 少了自研 Raft 的**正确性负债**，多了 etcd 的**部署运维**(但 etcd 运维成熟、文档/工具齐全，远好于自研排障)。
- 需引入 etcd C++ 客户端(gRPC + protobuf)——本项目已有 protobuf 工具链(`code/3party/protobuf`)，gRPC 是新增依赖，需评估。
- 跨网络: 节点 → etcd 是网络调用(注册/发号/Watch)，比节点内自研 Center 多一跳网络;但**配置读仍走本地 shm**，热路径不受影响。
- 写吞吐: etcd 每次写过 Raft + fsync，高频注册 churn 下吞吐有上限(万级 writes/s 量级)。Thunder 节点数少，远未触及，但需记录此边界。

---

## 8. 综合结论

| 维度 | 评估 |
|------|------|
| node_id 强一致(#2) | ✅ **etcd 原生解决(txn/CAS)，gossip 的死结在此解开** |
| 注册/健康检测(#1) | ✅ Lease + KeepAlive，租约过期自动摘除 |
| 路由分发(#3) | ✅ prefix Watch，收敛通常优于 gossip |
| 配置分发(#4) | ✅ revision 天然版本，无需自补反熵 |
| Admin 视图(#6) | ✅ 线性一致，无需降级 |
| 自研 Raft 正确性负债 | ✅ 外包给 etcd 消除 |
| shm 零跳直推 | ✅ Loader→shm→Worker 一跳完整保留 |
| "去掉组件"目标 | ❌ 未去掉(换成 etcd)，但消除了自研 Raft |
| 新增成本 | etcd 集群部署 + etcd C++(gRPC) 客户端依赖 |

**一句话结论**: 若诉求是"**消除自研 Raft 的正确性负债、保住 shm 零跳、且不放弃 snowflake 与运行时弹性**"，etcd 是比 gossip 更稳妥的路径——它不追求去中心化，而是把强一致这件难事外包给成熟件，并**恰好原生解决 gossip 卡死的 node_id 分配**。代价是引入(但不自研)一个外部协调集群。

---

## 9. 若要推进的渐进路线（建议，非承诺）

每步都在 `CenterConnector` 抽象层隔离，保证可回退:

1. **第一步(隔离层)**: 在 `CenterConnector` 下新增 `EtcdCenterConnector` 骨架，与现有自研 Center 实现并存，配置开关切换。
2. **第二步(发号验证)**: 先用 etcd txn 实现 `node_id` 分配(§3)，与现有发号逻辑做一致性对拍(同一集群双跑，校验无碰撞、槽位复用正确)。这是收益最大、最能体现 etcd 价值的一块。
3. **第三步(注册/路由)**: Lease 注册 + prefix Watch 路由，Watch 回调对接现有 shm 写入链路(version++/半包保护不动)。压测 Watch 收敛延迟与 etcd 写吞吐边界。
4. **第四步(配置)**: 配置 key + revision + Watch → 配置 shm。
5. **第五步(Admin)**: Admin 查询改走 etcd linearizable read。
6. 验证稳定后，下线自研 Raft / leader lease 相关代码。

**与 gossip 路线可并行评估**: 二者并非互斥——一种组合是 etcd 专管 #2 强一致发号(轻量、低频写)，gossip 管 #1/#3 高频成员/路由传播，各取所长。是否值得引两套，需看运维复杂度容忍度。

---

## 附录: 关键代码索引

- node_id 生成器: `code/Util/src/util/CommonUtils.hpp` (`GetUniqueId_MS` / `_LG` / `_S`)
- node_id 使用: `code/Net/include/labor/Labor.hpp` (`GetNodeId` / `GenerateUniqueId` / `GetNodeIdWokerId` / `m_uiNodeId`)
- 控制面抽象: `code/Net/include/labor/CenterConnector.hpp`(新增 `EtcdCenterConnector` 的落点)
- 在线表 / 订阅路由: `code/Center/src/SessionOnlineNodes.hpp`
- 注册 / 上报: `code/Center/src/CmdNodeRegister/`, `code/Center/src/CmdNodeReport/`
- Raft / leader lease(待外包消除): `docs/architecture/center_stagement.md`, `docs/architecture/raft_leader_lease_design.md`
- shm 直推优势拆解: `docs/architecture/center_vs_nacos_evaluation.md` §3.1
- 平行方案(去中心化): `docs/architecture/gossip_decentralization_evaluation.md`
