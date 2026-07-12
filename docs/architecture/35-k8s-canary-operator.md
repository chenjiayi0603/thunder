# Thunder 灰度发布运维工具链（CRD + Operator + CI + Admin UI）

> 设计日期：2026-07-08  |  状态：⏸️ 延后实施  |  关联：`34-k8s-canary-routing.md`（核心路由，已独立）
>
> **本文档描述的是运维工具链层，不阻塞核心路由功能。** 核心路由设计（~75 行 C++，etcd 权重键 + Worker 加权随机）见 `34-k8s-canary-routing.md`，可独立实现和上线。本文档的 CRD + Operator + CI + Admin UI 等运维设施在核心路由稳定后按需落地。

---

## 0. 为什么不用 K8s 原生方案

K8s 原生 **不支持权重流量**。Service 的负载均衡是内核级 iptables/IPVS 均匀分发，没有权重字段：

| 方案 | 原理 | 权重 | 额外组件 | 适用 |
|---|---|---|---|---|
| K8s Service | iptables/IPVS 均匀分发 | ❌ | 无 | 无权重场景 |
| nginx-ingress canary | L7 入口按 header/权重分流 | ✅ | Ingress Controller | 入口流量，不走内网 |
| Istio/Linkerd | sidecar 代理拦截 + 权重转发 | ✅ | 全套 Service Mesh | 运维复杂，性能损耗大 |
| **Thunder + etcd** | Worker 进程内选后端，读 etcd 权重 | ✅ | **零** | 内网 RPC + 网关，70 行代码 |

Thunder 的优势：etcd 本来就在用做服务发现，路由层本来就有 `GetNodeIdentify`，加权重就是一个 if/else。不需要 sidecar、不需要改网络层、不需要额外组件。

> **注意**：灰度的是 Logic（被调方），但路由选择发生在 Interface/HelloHttp 等**网关（主调方）**。一个 Logic 可能被多个网关调用——etcd 权重键是中心化的，所有上游网关 Worker 同时读同一份权重，不需要各自配置。

---

## 1. 目标

通过 Kubernetes CRD + Operator 模式实现 Thunder 网关的灰度发布，支持：

- **权重分流**：按百分比将流量切到新版本实例
- **秒级回滚**：改 etcd 权重即可，不杀 Pod
- **不中断**：旧实例继续服务剩余流量，新实例逐步接管

### 灰度全流程

```
                         ┌──────────────┐
                         │   用户操作    │
                         │ kubectl apply│
                         │ GrayRelease  │
                         └──────┬───────┘
                                │
                                ▼
              ┌─────────────────────────────────────┐
              │         Thunder Operator            │
              │         (Deployment Pod)            │
              │                                     │
              │  ┌─────────────────────────────┐    │
              │  │        Reconcile 循环        │    │
              │  │                              │    │
              │  │ ① 读 CRD spec                │    │
              │  │    service=LOGIC              │    │
              │  │    newVersion=v2              │    │
              │  │    weight=10                  │    │
              │  │         │                     │    │
              │  │         ▼                     │    │
              │  │ ② 新 Deployment 存在?         │    │
              │  │    ├─ 否 → kubectl apply v2   │    │
              │  │    │    ┌──────────────────┐  │    │
              │  │    │    │ 建 Deployment     │  │    │
              │  │    │    │ env: NODE_VERSION │  │    │
              │  │    │    │      =v2          │  │    │
              │  │    │    │ image: logic:v2   │  │    │
              │  │    │    └──────────────────┘  │    │
              │  │    │         │                 │    │
              │  │    │         ▼                 │    │
              │  │    │   等待 v2 Pod Ready       │    │
              │  │    └─ 是 → 继续               │    │
              │  │         │                     │    │
              │  │         ▼                     │    │
              │  │ ③ etcdPut weights             │    │
              │  │    /thunder/canary/LOGIC/      │    │
              │  │    weights                     │    │
              │  │    {"v1":90,"v2":10}           │    │
              │  │         │                     │    │
              │  │         ▼                     │    │
              │  │ ④ weight=100?                 │    │
              │  │    ├─ 否 → 等待下次 patch     │    │
              │  │    └─ 是 → sleep 30s          │    │
              │  │         → 旧 replicas=0        │    │
              │  │         → Completed            │    │
              │  └─────────────────────────────┘    │
              └────────────────┬────────────────────┘
                               │ etcd 变更
                               ▼
              ┌─────────────────────────────────────┐
              │              etcd                    │
              │                                     │
              │ /thunder/canary/LOGIC/weights       │
              │   {"v1":90,"v2":10}                 │
              │              │                      │
              │ /thunder/nodes/LOGIC/               │
              │   192.168.3.61:16068 (v1)           │
              │   192.168.3.61:16069 (v2)  ← 新增   │
              └────────────────┬────────────────────┘
                               │ Manager Watch (已有)
                               ▼
              ┌─────────────────────────────────────┐
              │          Thunder Manager            │
              │                                     │
              │  watch canary weights 变更           │
              │  AssembleAndPushRouteUpdated()      │
              │  → 写入共享内存 mirror               │
              └────────────────┬────────────────────┘
                               │ fork 时共享内存指针
                               ▼
              ┌─────────────────────────────────────┐
              │  所有上游网关 Worker (读同一份权重)   │
              │                                     │
              │  Interface Worker                   │
              │  HelloHttp Worker    ──── etcd ────  │
              │  HelloWs Worker                      │
              │    │                                 │
              │    │ 版本号变了 → 读最新 NodeNotice   │
              │    │ Nodes::GetNodeIdentify("LOGIC") │
              │    │   ├─ canary_weights 存在?       │
              │    │   │   ├─ v1 weight=90 → 90%     │
              │    │   │   └─ v2 weight=10 → 10%     │
              │    │   └─ 否 → 一致性哈希 (默认)      │
              │    │              │                   │
              │    │   请求 ────→ Logic v1/v2 后端     │
              └────┼─────────────┼───────────────────┘
                   │             │
                   ▼             ▼
              ┌────────┐   ┌────────┐
              │ Logic  │   │ Logic  │
              │  v1    │   │  v2    │
              │ 90%    │   │ 10%    │
              └────────┘   └────────┘

  ┌──────────────────────────────────────────────────────┐
  │                    回滚路径                           │
  │                                                      │
  │  kubectl patch weight=0                              │
  │    → Operator → etcd {"v1":100,"v2":0}              │
  │      → Manager Watch → Worker 下一笔请求只走 v1       │
  │                                                      │
  │  秒级生效，v2 Pod 保留不杀，随时 weight=10 恢复       │
  └──────────────────────────────────────────────────────┘
```

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────┐
│                  kubectl apply                   │
│  apiVersion: thunder.io/v1                       │
│  kind: GrayRelease                                │
│  spec:                                            │
│    service: LOGIC                                 │
│    newVersion: v2.0                               │
│    weight: 10      # 10% 流量到新版本             │
│    steps: [5,20,50,100]  # 灰度阶梯              │
└──────────────────────┬──────────────────────────┘
                       │ watch
┌──────────────────────▼──────────────────────────┐
│               GrayRelease Operator                │
│                                                    │
│  1. kubectl apply 新 Deployment (v2)              │
│  2. 等待 v2 Pod Ready                             │
│  3. 写 etcd 权重键                                 │
│     /thunder/canary/LOGIC/weights                │
│       v1: 90                                     │
│       v2: 10                                     │
│  4. 监控错误率，异常自动回滚                       │
└──────────────────────┬──────────────────────────┘
                       │ etcd put
┌──────────────────────▼──────────────────────────┐
│                      etcd                         │
│                                                    │
│  /thunder/nodes/LOGIC/  (服务注册，已有)           │
│  /thunder/canary/LOGIC/weights  (权重配置，新增)   │
└──────────────────────┬──────────────────────────┘
                       │ RouteUpdated (已有)
┌──────────────────────▼──────────────────────────┐
│              Thunder Gateway (Interface)          │
│                                                    │
│   Manager Watch etcd → 共享内存 mirror            │
│   Worker: GetNodeIdentify → 加权随机选后端         │
│   → 10% 请求 → v2 node, 90% → v1 node            │
└─────────────────────────────────────────────────┘
```

---

## 3. 触发方式

灰度的入口只有一个：**修改 CRD 的 `spec.weight` 字段**。Operator watch 到这个变化后自动执行。

> **一个 CRD 管一个服务类型的所有实例**，不是每个 Pod 一个。Logic 有 5 个 Pod？一个 CRD 搞定，`weight=10` 意思是 10% 流量分给 v2 那组 Pod，跟 v2 有几个副本无关。

```bash
# 方式一：直接 apply 完整的 CRD YAML
kubectl apply -f - <<EOF
apiVersion: thunder.io/v1
kind: GrayRelease
metadata:
  name: logic-v2
  namespace: thunder
spec:
  service: LOGIC
  newVersion: v2
  weight: 0      # 从 0 开始，先部署不走流量
EOF

# 方式二：patch 改权重（最常用）
kubectl patch gr logic-v2 -n thunder --type=merge -p '{"spec":{"weight":10}}'

# 方式三：kubectl edit 交互修改
kubectl edit gr logic-v2 -n thunder
```

`spec.weight` 是唯一需要改的字段。改它的一瞬间：

```
kubectl patch weight=10
  → Operator watch → 更新 etcd 权重键
    → Manager Watch → 共享内存 version++
      → Worker 下一笔请求开始按 10% 概率选 v2
```

不用 reload、不用重启、不用进 Pod 里面改配置。整个灰度流程就是反复 patch 这个数字：0→5→20→50→100。

---

## 4. 分层设计

### 4.1 CRD 定义

```yaml
apiVersion: apiextensions.k8s.io/v1
kind: CustomResourceDefinition
metadata:
  name: grayreleases.thunder.io
spec:
  group: thunder.io
  names:
    kind: GrayRelease
    singular: grayrelease
    plural: grayreleases
    shortNames: [gr]
  scope: Namespaced
  versions:
    - name: v1
      served: true
      storage: true
      schema:
        openAPIV3Schema:
          type: object
          required: [spec]
          properties:
            spec:
              type: object
              required: [service, newVersion, weight]
              properties:
                service:
                  type: string
                  description: 目标服务类型 (LOGIC/HELLO_HTTP/INTERFACE)
                newVersion:
                  type: string
                  description: 新版本标识 (v2.0 / canary-abc123)
                weight:
                  type: integer
                  minimum: 0
                  maximum: 100
                  description: 流向新版本的流量百分比
                steps:
                  type: array
                  items: { type: integer }
                  description: 灰度阶梯 [5,20,50,100]，为空则手动控制
                rollbackOnErrorRate:
                  type: number
                  description: 错误率阈值，超过自动回滚 (0.05 = 5%)
                rollbackOnLatencyP99:
                  type: string
                  description: P99 延迟阈值 (如 "500ms")
            status:
              type: object
              properties:
                phase:
                  type: string
                  enum: [Pending, Progressing, Running, Completed, RollingBack, RolledBack, Failed]
                currentWeight:
                  type: integer
                oldReplicas: { type: integer }
                newReplicas: { type: integer }
                message: { type: string }
```

### 4.2 Operator 逻辑

```
Reconcile 循环 (每次 CRD 变更触发):

1. 读 CRD spec
2. 如果新 Deployment 不存在 → 创建 (kubectl apply)
3. 等待新 Pod Ready
4. 写 etcd 权重键 → /thunder/canary/{service}/weights
5. 更新 CRD status.phase = Running
6. 持续监控 (Operator 直接查 Prometheus):
   - Envoy/Worker 上报指标 → Prometheus  ← 指标来源
   - Operator 定时查 Prometheus API    ← 监控判断
     · 错误率 > rollbackOnErrorRate → 自动回滚 (weight=0)
     · P99 延迟 > rollbackOnLatencyP99 → 自动回滚
   - 手动改 CRD weight → 立即更新 etcd
7. weight=100 时:
   a. 写 etcd weight=100 → 新请求不再路由到 v1
   b. sleep 30s → 存量短连接自然结束
   c. CRD status.phase = Completed
   d. 旧 Deployment 降 replicas=0（保留不删，手动确认后再 kubectl delete）
```

**回滚路径**：

```
手动回滚:  kubectl patch gr {name} --type=merge -p '{"spec":{"weight":0}}'
          → Operator watch → etcd weight=0 → 秒级生效

自动回滚:  Operator 检测错误率超标 → etcd weight=0 → CRD status.phase=RolledBack
```

### 4.3 Thunder 侧改动

#### 流量路由机制

一个请求从 Interface 到 Logic 后端，权重的完整传递链路：

```
┌─ Operator ─────────────────────────────────────────────────────┐
│ kubectl patch gr → Reconcile → etcdClient.Put(                 │
│   "/thunder/canary/LOGIC/weights",                              │
│   '{"v1":90,"v2":10}')                                          │
└──────────────────────┬─────────────────────────────────────────┘
                       │ etcd key 变更
┌─ EtcdGrpcConnector (已有) ─────────────────────────────────────┐
│ Watch 前缀 /thunder/canary/  (需新增，复用现有 Watch 框架)      │
│   ↓ OnWatchEvent                                               │
│ m_canaryWeights[LOGIC] = 解析后的权重 map                       │
│   ↓                                                            │
│ AssembleAndPushRouteUpdated()  (已有，需扩展)                    │
│   → NodeNotice protobuf 新增 canary_weights 字段                │
│   → SetNodeNotice() 写共享内存                                  │
│   → CenterEvent::RouteUpdated 推送 Manager                     │
└──────────────────────┬─────────────────────────────────────────┘
                       │ 共享内存 (已有 RouteNoticeVersionData)
┌─ Manager (已有) ───────────────────────────────────────────────┐
│ OnCenterEvent(RouteUpdated)                                    │
│   → 检查 route_snapshot 中的 canary_weights                     │
│   → SetNodeNotice() 写到共享内存 mirror                         │
│   → version++                                                  │
└──────────────────────┬─────────────────────────────────────────┘
                       │ fork 时 Worker 拿到同一块共享内存指针
┌─ Worker (已有) ────────────────────────────────────────────────┐
│ 事件循环每轮:                                                    │
│   GetRouteNoticeVersionData().GetNodeNoticeVersion()  ≠ 本地?   │
│     → 版本变了，GetNodeNotice() 读到最新 NodeNotice              │
│       → 解析出 canary_weights                                   │
│                                                                 │
│ 请求到来，需要路由到 Logic:                                      │
│   Nodes::GetNodeIdentify("LOGIC", hashKey)  ← 只改这一处        │
│     ├─ canary_weights["LOGIC"] 存在?                            │
│     │   ├─ 是 → 加权随机                                        │
│     │   │       v1 weight=90 → 90%                                │
│     │   │       v2 weight=10 → 10%                                │
│     │   └─ 否 → 现有一致性哈希 (不变)                            │
│     └─ 返回选中 node 的 ip:port                                 │
│                                                                 │
│ Worker 发起内部连接 → 请求到达目标 Logic Worker                   │
└─────────────────────────────────────────────────────────────────┘
```

**全链改动清单**：

| 模块 | 改动 | 量 |
|---|---|---|
| 节点注册 | 读 `NODE_VERSION` 环境变量，注册时写入 etcd | +5 行 |
| etcd Watch | 新增 `/thunder/canary/` 前缀 watch | +30 行 |
| protobuf | `NodeNotice` 加 `canary_weights` map + `node_version` 字段 | +5 行 |
| EtcdGrpcConnector | `OnWatchEvent` 解析权重, `AssembleAndPushRouteUpdated` 按 version 对权重 | +20 行 |
| Nodes | `GetNodeIdentify` 加权随机分支 | +15 行 |

**总代码量：~75 行。** 打 version tag 就是 Deployment YAML 里加一行 env `NODE_VERSION=v2`，共享内存/watch/版本号全复用现有机制。

**需改代码的地方只有一处**：`Nodes::GetNodeIdentify` 的路由选择逻辑。

现有 `Nodes::GetNodeIdentify(strNodeType, strHashKey)` 使用一致性哈希选后端。灰度模式下需改为加权随机：

```cpp
// 伪代码 — 改动点在 code/Net/src/dispatcher/Nodes.cpp
const std::string& Nodes::GetNodeIdentify(
    const std::string& strNodeType, 
    const std::string& strHashKey)
{
    // 读 etcd 权重（已有共享内存 mirror 机制，直接复用）
    auto& weights = GetCanaryWeights(strNodeType);
    
    if (weights.empty()) {
        // 没有灰度配置 → 走现有一致性哈希
        return GetNodeIdentifyByHash(strNodeType, strHashKey);
    }
    
    // 有灰度配置 → 加权随机
    int totalWeight = 0;
    for (auto& [node, w] : weights) totalWeight += w;
    
    int randVal = GetWorkerLocalRandom() % totalWeight;
    int acc = 0;
    for (auto& [node, w] : weights) {
        acc += w;
        if (randVal < acc) return node;
    }
    return weights.begin()->first;  // fallback
}
```

**不需要改的地方**：

| 模块 | 原因 |
|---|---|
| etcd watch | 已有 `RouteUpdated` 事件，Manager 自动 watch |
| 共享内存 mirror | 已有 `RouteNoticeVersionData`，push 到 Worker 零拷贝 |
| 热重载 | 已有 `GracefulRestartWorker`，权重变更不需要重启 |
| 服务注册 | 已有 etcd-grpc `DoRegisterGrpc`，新节点自动注册 |

---

## 5. etcd 键设计

```
/thunder/canary/{NODE_TYPE}/weights
  → {"v1":90,"v2":10}

// 极简。version tag 跟节点注册时的 node_version 字段对应。
// Operator 改这个键，Manager Watch 到了就推给 Worker。
```

节点注册时也要带 version：

```
/thunder/nodes/LOGIC/192.168.3.61:16068
  → {"node_id":80,"node_type":"LOGIC","node_version":"v1",...}
                                                 ↑ 新增
```

`AssembleAndPushRouteUpdated` 拼路由表时，拿每个节点的 `node_version` 去权重 map 里对：

```
node_id=80, version=v1 → canary_weights["v1"]=90 → 权重=90
node_id=81, version=v2 → canary_weights["v2"]=10 → 权重=10
```

**为什么按 version 不按 node_id**：

| | 按 node_id | 按 version |
|---|---|---|
| v2 扩容 2→10 副本 | 权重键要加 8 个 node_id | **不用改**，新节点注册时自带 version=v2 |
| 权重键内容 | `{"80":90,"81":10,"82":10,...}` | `{"v1":90,"v2":10}` |
| 新增 v3 灰度 | 元数据无感知，手动维护 | 新部署自带 version=v3，自动入组 |
- **version**：每次权重变更 +1，Worker 对比本地版本号决定是否重读
- **weight=0**：表示该节点不接收灰度流量（但 Deployment 保留，随时可恢复）

---

## 6. 灰度流程示例

```bash
# 1. 部署新版本 Logic (不接流量)
kubectl apply -f - <<EOF
apiVersion: thunder.io/v1
kind: GrayRelease
metadata:
  name: logic-v2-canary
  namespace: thunder
spec:
  service: LOGIC
  newVersion: v2.0
  weight: 0        # 先部署，不走流量
  steps: [5, 20, 50, 100]
EOF

# 2. 开始灰度：5% 流量 → v2
kubectl patch gr logic-v2-canary --type=merge -p '{"spec":{"weight":5}}'

# 3. 观察监控，逐步放量
kubectl patch gr logic-v2-canary --type=merge -p '{"spec":{"weight":20}}'
kubectl patch gr logic-v2-canary --type=merge -p '{"spec":{"weight":50}}'

# 4. 全量切换
kubectl patch gr logic-v2-canary --type=merge -p '{"spec":{"weight":100}}'

# 5. 出问题 → 秒级回滚
kubectl patch gr logic-v2-canary --type=merge -p '{"spec":{"weight":0}}'
```

---

## 7. 实现计划

| 阶段 | 内容 | 预估 |
|------|------|:--:|
| **P0：节点权重注册** | etcd 增加权重字段，`AssembleAndPushRouteUpdated` 携带权重 | 1d |
| **P1：加权路由** | `Nodes::GetNodeIdentify` 支持加权随机模式，by hash key 的一致性哈希仍为默认 | 2d |
| **P2：CRD + Operator** | 定义 CRD，写 Go Operator (kubebuilder)，watch CRD → etcd | 3d |
| **P3：自动回滚** | ⚠️ 需 Prometheus 就绪（etcd 只有连接指标，无业务错误率/延迟） | 后续 |
| **P4：集成测试** | kubeadm 集群上全流程验证（手动 `kubectl patch weight=0` 回滚） | 1d |
| **P5：文档 + 示例** | 灰度发布运维手册 + 示例 YAML | 0.5d |
| **P6：admin 接口 + 可视化** | 复用 admin-web，暴露 `GET/PUT /api/canary/{service}` HTTP 接口 + Web 滑块调权重 | 2d |

---

## 附录 A：与 Istio 灰度方案对比

### A.1 相同理念，不同实现

两者都按 **version tag 分组** 做权重流量——Istio 叫 `subset`，Thunder 叫 `node_version`。

```
Istio:                    Thunder:
VirtualService             GrayRelease CRD
  ├─ subset: v1  90%        ├─ v1: 90%
  └─ subset: v2  10%        └─ v2: 10%
         ↓                        ↓
DestinationRule             etcd /thunder/canary/weights
  v1 → label version=v1       v1 → node_version=v1
  v2 → label version=v2       v2 → node_version=v2
         ↓                        ↓
     Envoy sidecar            Thunder Worker 进程内路由
```

### A.2 数据路径对比

```
Istio:
  请求 → iptables 劫持 → Envoy sidecar → 网络 → 对端 Envoy → 应用
          ↑ 额外一跳           ↑ 加解密/重试        ↑ 额外一跳

Thunder:
  请求 → Thunder Worker → 读 etcd 权重 → 选后端 → 直连
          ↑ 零代理，进程内路由
```

| | Istio | Thunder + etcd |
|---|---|---|
| **架构** | sidecar 代理（每个 Pod 一个 Envoy） | 进程内路由，零代理 |
| **生效方式** | iptables 劫持流量 → Envoy 转发 | Worker 代码内 `GetNodeIdentify` 加权 |
| **额外延迟** | 每跳 +2~10ms（两次代理穿透） | **0** |
| **CPU 开销** | Envoy 每 Pod 额外 0.2~0.5 核 | **0** |
| **内存开销** | Envoy 每 Pod 额外 50~200MB | **0** |
| **组件** | Istiod + Envoy × N Pod | Thunder 自身（etcd 已有） |
| **配置** | VirtualService + DestinationRule + Gateway | 1 个 CRD 或 1 个 ConfigMap |
| **Scope** | 集群内任意 HTTP/gRPC 服务 | Thunder 集群内服务 |
| **新服务接入** | 注入 sidecar，改 Service 配置 | 加 1 行 env `NODE_VERSION=v2` |
| **运维复杂度** | 高（istiod HA、Envoy 版本升级、mTLS 证书轮换） | 低（etcd 已在运维） |
| **适用场景** | 全集群统一治理、多语言异构服务 | Thunder 专属网关 + RPC 框架 |

### A.3 Operator 操作方式对比

> Istio 本身不包含灰度 Operator，但可以通过 **Flagger**（Istio 生态的灰度 Operator）实现类似体验。Flagger 提供了 `Canary` CRD，背后自动管理 VirtualService + DestinationRule + Deployment。这里把三种方式放在一起对比。

三种方式：
- **Istio 原生**：手动管理 VirtualService + DestinationRule + Deployment（3 个 YAML）
- **Istio + Flagger**：1 个 Canary CRD，Flagger Operator 自动编排（类似 Thunder）
- **Thunder**：1 个 GrayRelease CRD，自写 Operator

#### 创建灰度

```bash
# ── Istio 原生 ─────────────────────────────────
# 需要 3 个 YAML 手动编排：Deployment + VirtualService + DestinationRule
kubectl apply -f logic-v2.yaml

kubectl apply -f - <<EOF
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
spec:
  http:
  - route:
    - destination: {host: logic, subset: v1}
      weight: 90
    - destination: {host: logic, subset: v2}
      weight: 10
---
apiVersion: networking.istio.io/v1beta1
kind: DestinationRule
spec:
  subsets:
  - name: v1
    labels: {version: v1}
  - name: v2
    labels: {version: v2}
EOF

# ── Istio + Flagger ─────────────────────────────
# 1 个 Canary CRD，Flagger Operator 自动建 Deployment + VS + DR + 逐步调权重
kubectl apply -f - <<EOF
apiVersion: flagger.app/v1beta1
kind: Canary
metadata:
  name: logic
  namespace: thunder
spec:
  targetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: logic
  service:
    port: 16068
  analysis:
    interval: 30s           # 每 30s 调一次权重
    threshold: 5             # 最多 5 次失败 → 自动回滚
    maxWeight: 50            # 最大灰度到 50%
    stepWeight: 10           # 每次 +10%
    metrics:                 # 监控指标
    - name: request-success-rate
      thresholdRange:
        min: 99              # 成功率 <99% → 回滚
    - name: request-duration
      thresholdRange:
        max: 500             # P99 >500ms → 回滚
EOF
# Flagger Operator 接下来自动:
#   1. 创建 logic-primary Deployment (稳定版)
#   2. 每次 +10% 权重, 间隔 30s
#   3. 监控 Prometheus, 超标自动 rollback
#   4. 用户不需要手动改 VirtualService —— Operator 全自动

# ── Thunder ────────────────────────────────────
# 1 个 GrayRelease CRD，自写 Operator 自动建 Deployment + 写 etcd 权重
kubectl apply -f - <<EOF
apiVersion: thunder.io/v1
kind: GrayRelease
spec:
  service: LOGIC
  newVersion: v2
  weight: 10
EOF
```

#### 调整权重

```bash
# Istio:  修改 VirtualService，istiod 通过 xDS 推送 → Envoy 热加载
kubectl patch vs logic-canary --type=json \
  -p='[{"op":"replace","path":"/spec/http/0/route/0/weight","value":20},

# Flagger: CLI 或 patch Canary
kubectl -n thunder set canary/logic weight=20

# Thunder: patch 一个数字
kubectl patch gr logic-v2 -n thunder --type=merge -p '{"spec":{"weight":20}}'
```

#### 回滚

```bash
# Istio:  改 VirtualService weight → istiod 推送
kubectl patch vs logic-canary --type=json \
  -p='[{"op":"replace","path":"/spec/http/0/route/0/weight","value":100},
       {"op":"replace","path":"/spec/http/0/route/1/weight","value":0}]'

# Flagger: 若超标自动回滚（analysis.metrics 触发），或手动
kubectl -n thunder set canary/logic weight=0

# Thunder: 改一个数字，秒级生效
kubectl patch gr logic-v2 -n thunder --type=merge -p '{"spec":{"weight":0}}'
```

#### 对比总结

| 操作 | Istio 原生 | Istio + Flagger | Thunder |
|---|---|---|---|
| **初始部署** | 3 步（Deployment + VS + DR） | **1 步**（apply Canary CRD） | **1 步**（apply GrayRelease CRD） |
| **调整权重** | patch VS，改 2 个 weight | `kubectl set canary weight 20` | **patch 1 个数字** |
| **回滚** | patch VS 恢复 | `kubectl set canary weight 0` | **patch 1 个数字** |
| **配置传播** | istiod → xDS → Envoy | istiod → xDS → Envoy | Operator → etcd → 共享内存 |
| **生效延迟** | <1s | <1s | <1s |
| **额外组件** | istiod + Envoy | istiod + Envoy + Flagger | **etcd（已有）** |
| **数据面开销** | sidecar 代理（+4ms） | sidecar 代理（+4ms） | **0** |

### A.4 Operator 是什么

Operator 不是一个具体软件，是一种**模式**：CRD + Controller。

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Control Plane (Master)                        │
│                                                                      │
│  ┌──────────────────────────┐    ┌──────────────────────────────┐   │
│  │ kube-controller-manager  │    │          etcd                │   │
│  │  (宿主机二进制, 非 Pod)   │    │  (宿主机二进制/Static Pod)    │   │
│  │                          │    │                              │   │
│  │ ┌──────────────────────┐ │    │  所有 CRD 资源都存在这        │   │
│  │ │ Deployment Controller│ │    │  Deployment / Service /      │   │
│  │ │  while true:         │ │    │  Canary / GrayRelease / ... │   │
│  │ │    replicas=3 ?      │ │    └──────────────────────────────┘   │
│  │ │    → 创建/删除 Pod    │ │                                       │
│  │ └──────────────────────┘ │                                       │
│  │ ┌──────────────────────┐ │                                       │
│  │ │ ReplicaSet Controller│ │                                       │
│  │ │ Node Controller ...  │ │                                       │
│  │ └──────────────────────┘ │                                       │
│  └──────────────────────────┘                                       │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                        Worker Node                                   │
│                                                                      │
│  ┌──────────────────────┐    ┌──────────────────────────────┐       │
│  │  Pod: flagger        │    │  Pod: thunder-operator       │       │
│  │  (Deployment 部署)    │    │  (Deployment 部署)           │       │
│  │                      │    │                              │       │
│  │  while true:         │    │  while true:                 │       │
│  │    watch Canary CRD  │    │    watch GrayRelease CRD     │       │
│  │    → 建 Deployment   │    │    → 建 Deployment           │       │
│  │    → 调 VS weight    │    │    → 写 etcd weight          │       │
│  │    → 查 Prometheus   │    │    → 查 Prometheus           │       │
│  │    → 超标自动回滚    │    │    → 超标自动回滚            │       │
│  └──────────────────────┘    └──────────────────────────────┘       │
└─────────────────────────────────────────────────────────────────────┘
```

**三个 Operator，模式相同，管的东西不同**：

| Operator | 跑在哪 | Watch 什么 CRD | 操作什么目标 | K8s 自带？ |
|---|---|---|---|---|
| Deployment Controller | control-plane 宿主机进程 | Deployment | 创建/删除 Pod | ✅ 内置 |
| Flagger | 集群内 Pod | Canary | 改 VirtualService, Deployment | ❌ 社区安装 |
| Thunder Operator | 集群内 Pod | GrayRelease | 写 etcd 权重, Deployment | ❌ 自己写 |

#### Canary CRD 是怎么被执行起来的

Canary CRD 自己不干任何事——它只是一份存在 etcd 里的 YAML。要让流量真正按权重走，需要**两条独立的 watch 链路接力**：

```
用户: kubectl apply Canary (weight=10)
         │
         ▼ etcd 存一份 YAML
         │
    ┌────┴────────────────────────────────────────────┐
    │                                                   │
    ▼ watch CRD                 (另一条链路)            ▼ watch CRD
┌─ Flagger ───────────────┐                  ┌─ istiod ──────────────┐
│                          │                  │                       │
│ ① 读 Canary.spec        │                  │ ④ 读 VirtualService   │
│    → 创建 v2 Deployment  │                  │    weight: v1=90,v2=10│
│    → 创建 VirtualService │─── K8s API ───→ │                        │
│       weight: v1=90,v2=10│                  │ ⑤ xDS 推配置          │
│    → 创建 DestinationRule│                  │    → Envoy sidecar    │
│                          │                  │      routing table    │
│ ② 等待 v2 Pod Ready      │                  │      weight: 90/10    │
│                          │                  │                       │
│ ③ 每 30s 查 Prometheus:  │                  │ ⑥ Envoy 更新路由表    │
│    成功率 >99%?→ weight+10│                 │    iptables 劫持流量  │
│    成功率 <99%?→ weight=0 │                  │    → 真正按权重分流   │
└──────────────────────────┘                  └───────────────────────┘
    Flagger 只管写 CRD                             istiod 只管推 Envoy
    (改的是 etcd 里的 YAML)                      (改的是 Envoy 内存)
```

**Flagger 和 Envoy 之间没有任何直接通信。** Flagger 改了 VirtualService CRD 后就完事了——istiod 在另一条链路上独立 watch，发现 CRD 变了才推给 Envoy。中间隔了 K8s API Server + etcd。

**Thunder 对比**——链路更短，因为 Worker 自己就是 Envoy 的替代：

```
用户: kubectl apply GrayRelease (weight=10)
         │
         ▼
┌─ Thunder Operator ────────┐
│                            │
│ ① 读 GrayRelease.spec     │
│    → 创建 v2 Deployment    │
│    → etcdPut weights       │──── etcd ────→ Manager Watch ──→ Worker
│      {"v1":90,"v2":10}    │                      ↑
│                            │              没有中间层，Worker 直接读
│ ② 查 Prometheus 指标      │
│    超标→ weight=0         │
└────────────────────────────┘
```

| | Istio (Flagger) | Thunder |
|---|---|---|
| **控制面接力次数** | Flagger → K8s API → istiod → xDS → Envoy (5 层) | Operator → etcd → Worker (**2 层**) |
| **Flagger 的职责边界** | 只写 CRD，不碰 Envoy | Operator 写 etcd，Worker 直接读 |
| **谁真正执行权重分流** | Envoy（sidecar 代理） | Thunder Worker（进程内路由） |
| **数据路径** | iptables → Envoy → 网络 → Envoy → 应用 | Worker → 直连后端 |

#### 监控数据从哪来，Operator 跑在哪

Flagger 做自动回滚需要**错误率/延迟**数据，这些数据不是 K8s Controller 提供的，是 Prometheus 自己去抓的：

```
┌─ Envoy sidecar ──────────────┐
│ :15090/stats/prometheus      │  ← 每个 Pod 里的 Envoy 暴露指标 HTTP 端点
│   istio_requests_total{      │
│     response_code="200"      │
│   }                          │
│   istio_request_duration_    │
│     milliseconds_bucket{...} │
└──────────┬───────────────────┘
           │ Prometheus 定时 scrape (每 15s)
           ▼
┌─ Prometheus Pod ─────────────┐
│ (跑在 Worker Node)            │
│  TSDB 存储时间序列             │
│  rate(requests_total[1m])     │
│  histogram_quantile(0.99,...) │
└──────────┬───────────────────┘
           │ Flagger 定时查 Prometheus API
           │ GET /api/v1/query?query=...
           ▼
┌─ Flagger Pod ────────────────┐
│ (跑在 Worker Node)            │
│ 查 Prom:                      │
│   成功率 = 200 / (200+500)   │
│   if 成功率 < 99%:           │
│     weight = 0 (回滚)        │
│     kubectl patch VS         │
└──────────────────────────────┘
```

**三层都跑在 Worker Node 上，都是 Pod**：

```
Worker Node:
  ├─ Pod: logic-v1
  │   └─ Envoy sidecar  ← 暴露 :15090/stats/prometheus
  ├─ Pod: logic-v2
  │   └─ Envoy sidecar
  ├─ Pod: prometheus     ← 抓 Envoy 指标，存 TSDB
  └─ Pod: flagger        ← 查 Prometheus，决定回滚
         (Deployment, 通常 ns=flagger-system)
```

Flagger 不是跑在 control-plane 上的特殊进程——就是一个普通 Deployment Pod，跟你的 logic 服务一样调度到 Worker Node。它跟 Prometheus 之间只是 HTTP 请求，跟 Envoy 之间没有直接通信（中间隔了 Prometheus）。

Thunder 要做的 Operator 完全同理：一个 Deployment Pod，查指标来源（Prometheus 或 etcd），超标就改 etcd 权重。

### A.5 为什么 Istio 不自带灰度 Operator，而 Thunder 要自己写

Istio 的职责边界是**网络层**：
- 定义流量规则（VirtualService → 权重、重试、超时）
- 定义服务子集（DestinationRule → subset 到 label 的映射）
- 下发配置到 Envoy（istiod → xDS）

灰度编排（Canary Release）是**应用层**的事——创建新 Deployment、逐步调权重、监控指标、自动回滚。这些不属于网络层，Istio 把这部分留给生态工具（Flagger、Argo Rollouts）。

```
        Istio (网络层)     Flagger (编排层)
        ──────────────     ────────────────
        VirtualService     Canary CRD
        DestinationRule    → 建新 Deployment
        istiod → Envoy     → 逐步改权重
                           → 监控 + 自动回滚
                           → 改的是 VirtualService 的 weight
```

Thunder 自己写 Operator 是因为**网络层不是 Envoy**——路由在 etcd + Worker 进程内部。Flagger 不认识 Thunder 的 etcd 权重键格式。所以 Operator 做的是 Flagger 的活，操作的是 Thunder 的 etcd。

```
        Thunder 路由层     Thunder Operator (编排层)
        ───────────────     ────────────────────────
        etcd 权重键         GrayRelease CRD
        Manager Watch       → 建新 Deployment  
        共享内存 mirror     → 逐步改 etcd weight
        Worker 加权路由     → 监控 + 自动回滚
```

### A.6 为什么 Thunder 不用 Istio

1. **性能**：Thunder HTTP 64B 220μs。加 Envoy 两次代理穿透至少多 4ms——20 倍性能损失
2. **已有 etcd**：服务发现、配置下发、Watch 热更新已全套基于 etcd，再加 Istio 等于引入第二套控制面
3. **Thunder 是网关**：Istio 的 Ingress Gateway 本身也是 Envoy，Thunder 比它更快，不需要在前面再加一层
4. **范围匹配**：Thunder 集群内全是同构服务，不需要 Istio 的跨语言、跨协议通用治理能力
