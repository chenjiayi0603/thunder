# 加权路由灰度 — etcd 权重 + Worker 进程内分流

> 仅通过 etcd 一个权重键 + ~75 行 C++ 实现按百分比灰度分流

---

## 为什么不用 K8s 原生方案

K8s Service 的 iptables/IPVS 均匀分发，**不支持权重**：

| 方案 | 原理 | 权重 | 额外组件 | 适用 |
|---|---|---|---|---|
| K8s Service | iptables/IPVS 均匀分发 | ❌ | 无 | 无权重场景 |
| nginx-ingress canary | L7 入口按 header/权重分流 | ✅ | Ingress Controller | 入口流量 |
| Istio/Linkerd | sidecar 代理拦截 + 权重转发 | ✅ | 全套 Service Mesh | 运维复杂，性能损耗 |
| **Thunder + etcd** | Worker 进程内选后端，读 etcd 权重 | ✅ | **零** | 内网 RPC + 网关 |

etcd 本来就在用，路由层本来就有 `GetNodeIdentify`，加权重就是一个 if/else。零额外组件。

---

## 数据流

```
┌─ 运维操作 ──────────────────────────────────────────────────┐
│  etcdctl put /thunder/canary/LOGIC/weights '{"v1":90,"v2":10}'│
└──────────────────────┬───────────────────────────────────────┘
                       │
┌─ EtcdCenterConnector (Watch /thunder/canary/ 前缀) ─────────┐
│ Watch 回调 → 解析权重 → m_canaryWeights[LOGIC] = {"v1":90,"v2":10} │
│ → AssembleAndPushRouteUpdated() → NodeNotice              │
│ → SetNodeNotice() 写共享内存                               │
└──────────────────────┬───────────────────────────────────────┘
                       │ 共享内存
┌─ Worker RouteNotice ───────────────────────────────────────┐
│ CheckShareMem → ReadRouteMirror → m_canaryWeights          │
└──────────────────────┬───────────────────────────────────────┘
                       │
┌─ Worker 请求处理 — 加权选择后端 ─────────────────────────────┐
│ GetNodeIdentify("LOGIC"):                                   │
│   1. 候选列表: Logic 节点按 version 分组 → v1×3, v2×1       │
│   2. 查权重: v1=90%, v2=10%                                 │
│   3. rand()%100 < 90 → 选 v1; else → 选 v2                 │
│   4. 从选中的分组里轮询/最小负载选出具体节点                  │
│   5. 灰度的是 Logic（被调方），选择发生在 Interface/Hello（主调方）│
└──────────────────────────────────────────────────────────────┘
```

---

## 核心算法

```cpp
// GetNodeIdentify — 加权选择
NodeIdentify GetNodeIdentify(const string& nodeType) {
    auto candidates = m_nodesMgr.FindByType(nodeType);  // 所有 Logic 节点
    auto groups = GroupByVersion(candidates);            // v1→[3nodes], v2→[1node]

    auto weights = m_canaryWeights[nodeType];            // {"v1":90, "v2":10}
    if (weights.empty()) return RoundRobin(groups);       // 无权重 → 均匀轮询

    int r = rand() % 100;
    int cumulative = 0;
    for (auto& [ver, weight] : weights) {
        cumulative += weight;
        if (r < cumulative) {
            // 从这个 version 分组里选一个节点（轮询/最小负载）
            return PickNode(groups[ver]);
        }
    }
}
```

---

## etcd 权重键格式

```
/thunder/canary/{NODE_TYPE}/weights
  value: {"v1":90, "v2":10}

/thunder/canary/LOGIC/weights = {"v1":90, "v2":10}
/thunder/canary/HELLO_HTTP/weights = {"v1":80, "v2":20}
```

---

## 回滚

```
改权重即可，不杀 Pod，秒级生效:
  etcdctl put /thunder/canary/LOGIC/weights '{"v1":100,"v2":0}'
```

- 旧实例继续服务剩余流量，新实例逐步接管
- Worker 读共享内存，权重变更实时生效
- 不依赖 DNS、不依赖 K8s Service 更新
