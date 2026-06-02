# k8s 场景下 Center 何去何从评估

> 日期: 2026-06-03
> 状态: 评估 / 技术探索(未落地)
> 驱动问题: 假如将来用 k8s 运维 Thunder,能否直接用 k8s 的 etcd 做注册中心?Center 还需要吗?
> 结论先行: **不能蹭 k8s 自带的 etcd(它是集群命根子,托管 k8s 也不给访问)。在 k8s 上有两条路:① 自建独立 etcd;② k8s-native(用 Service/ConfigMap/Lease/StatefulSet 等原语)。后者更划算——k8s 原生吃掉服务发现/健康检查/配置/选主,Center 缩到只剩 node_id,而 StatefulSet 序号正好能当 node_id。上了 k8s,很可能不再需要独立 Center。**

> 关联评估: [etcd 替代方案](./etcd_as_center_evaluation.md) · [NuRaft 接入设计](./nuraft_center_integration_design.md) · [Center vs Nacos](./center_vs_nacos_evaluation.md) · [gossip 去中心化](./gossip_decentralization_evaluation.md)

---

## 1. 为什么不能直接连 k8s 的 etcd

k8s 的 etcd **专供 k8s 控制面(apiserver)**,业务**严禁直连**:

```
       ✗ 业务 ──直连──► k8s 的 etcd      ← 反模式, 禁止

       ✓ k8s etcd 只给 apiserver:
  kube-apiserver ──唯一读写──► k8s etcd(集群命根子)
       ▲
  其他组件(scheduler/controller/kubelet)都经 apiserver, 没人直连 etcd
```

| 原因 | 说明 |
|---|---|
| **爆炸半径 = 全集群** | etcd 存整个集群状态。业务狂写/狂 watch/塞大 value → 拖垮 apiserver → **整个 k8s 集群挂** |
| **托管 k8s 不给访问** | EKS/GKE/AKS **根本不暴露 etcd**;自建集群 etcd 也锁 mTLS、只在控制面节点、pod 网络够不着 |
| **资源/版本耦合** | etcd 有配额(~2-8GB)、压缩/版本由 k8s 管,业务数据跟 k8s 对象抢空间、被升级牵着走 |

---

## 2. 在 k8s 上的两条路

### 路 A: 自建独立 etcd
- 用 etcd-operator / StatefulSet / bitnami chart 起一套**独立 etcd**,与 k8s 控制面 etcd 完全隔离。
- 拿到 etcd 全套能力(txn/lease/watch),互不影响。
- 适合: 想最小改动沿用 [etcd 评估](./etcd_as_center_evaluation.md) 的方案,只是把 etcd 跑在 k8s 里。

### 路 B(更划算): k8s-native
不直连 etcd,而是**经 apiserver 用 k8s 原语**(安全、官方支持的路径)。此时**Center 的活大半被 k8s 原生吃掉**:

| Center 现有职责 | k8s-native 替代 | 一致性 |
|---|---|---|
| 服务注册 / 发现 | **Service + Endpoints / EndpointSlice**(原生服务发现 + DNS) | k8s 维护 |
| 健康检查 | **liveness / readiness probe**(自带) | k8s 维护 |
| 配置下发 | **ConfigMap / Secret**(挂载文件 或 watch) | apiserver watch |
| 选主 | **Lease 对象**(coordination.k8s.io,k8s 控制器自身就用它选主)/ client-go leaderelection | 强一致(底层 etcd) |
| 自定义状态 | **CRD + controller** | 强一致 |
| **node_id 分配** | **k8s 无现成,残留**(见 §3) | — |

> 这些 k8s 原语底层**也是存进 k8s 的 etcd**,但是**经 apiserver 间接访问**(正确路径),不是业务直连 etcd。

---

## 3. 残留问题: node_id 在 k8s 上怎么解

服务发现/健康/配置/选主都被 k8s 吃掉后,**只剩 node_id 分配**是 k8s 不直接提供的。两种解法:

### 3.1 StatefulSet 序号(优雅,推荐)
- **StatefulSet 给每个 pod 一个稳定序号 `0,1,2...`** —— 这正好是"密集小整数唯一 ID"。
- `pod-7` → node_id=7,**天然稳定、不重复、重启不变、不用任何分配逻辑**。
- 对照 [gossip 评估](./gossip_decentralization_evaluation.md) §3:node_id 要求"全局唯一 + 密集稀缺槽位",StatefulSet 序号**原生满足**,且无需共识发号——k8s 的 StatefulSet 控制器保证序号唯一。
- 限制: node_id 上限受 StatefulSet 副本数约束;序号回收语义需结合 `NODE_ID_MAX`(255)评估。

### 3.2 CRD + controller(灵活)
- 定义一个 `NodeId` CRD,写一个 controller 用 etcd 强一致(经 apiserver)发号——等价把 [etcd 评估](./etcd_as_center_evaluation.md) §3 的 txn 发号逻辑搬进 controller。
- 适合: 需要槽位复用、动态分配、超出 StatefulSet 序号语义的场景。

---

## 4. shm 零跳不受影响

无论配置源是 Center / etcd / ConfigMap:

```
ConfigMap 变更 → pod 内 Loader watch 到 → 写 shm → Worker 本地读(纳秒)
```

**shm 零跳是 pod 内部优化,与配置源无关**,k8s 场景照样保留(见 [center_vs_nacos_evaluation.md](./center_vs_nacos_evaluation.md) §3.1)。

---

## 5. 结论

| 做法 | 评价 |
|---|---|
| 直连 k8s 的 etcd | ❌ 禁止(反模式 + 托管 k8s 不给访问) |
| 自建独立 etcd | ✅ 可行,拿到 etcd 全套,与控制面隔离;沿用 etcd 评估方案 |
| **k8s-native(Service/ConfigMap/Lease/StatefulSet)** | ✅✅ **最划算**:Center 大半功能被 k8s 原生吃掉,只剩 node_id,StatefulSet 序号即可解 |

**一句话**: 上了 k8s,**不能蹭 k8s 的 etcd**;要么自建独立 etcd,要么走 k8s-native。后者更优,因为服务发现/健康/配置/选主 k8s 全包,**Center 缩到只剩 node_id,而 StatefulSet 序号正好当 node_id**——届时**很可能不再需要独立 Center**。

> 与自研路线的关系: 自研 Center / 嵌 NuRaft / 外部 etcd 是"不上 k8s"时的选项;一旦上 k8s,k8s-native 让 Center 几乎消解。这意味着**在 NuRaft 上投入大量自研共识工作,可能被未来的 k8s 化抵消**——这是评估"是否值得现在嵌 NuRaft"时要权衡的一点。

---

## 附录: 关键对照

- node_id 唯一性要求: `docs/architecture/gossip_decentralization_evaluation.md` §3(全局唯一 + 密集稀缺)
- etcd 发号/lease/watch 能力: `docs/architecture/etcd_as_center_evaluation.md` §3/§4/§8
- shm 零跳: `docs/architecture/center_vs_nacos_evaluation.md` §3.1
- 现状 Center 职责盘点: `docs/architecture/gossip_decentralization_evaluation.md` §2
