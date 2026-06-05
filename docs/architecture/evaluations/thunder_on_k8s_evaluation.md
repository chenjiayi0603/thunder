# Thunder 在 k8s 上的适应性分析

> 日期: 2026-06-05
> 状态: 评估
> 驱动问题: Thunder 作为高性能异步网关框架,是否适合放在 k8s 上多节点部署?

---

## 1. Thunder 的架构画像

```
┌──────────────── 单台机器 ────────────────┐
│  Manager(W0)                              │
│   ├─ Worker(W1) ── 处理请求               │
│   ├─ Worker(W2)   (多进程, shm IPC)       │
│   └─ 监听端口(16068/27006/...)             │
│                                            │
│  etcd(127.0.0.1:2379) ── 注册/发现/配置    │
└──────────────────────────────────────────┘

跨机通信:
  Machine-A:Logic ──TCP──► Machine-B:Hello    (etcd 注册表 → IP:port 直连)
  Manager ──HttpCodec──► etcd                  (异步连接, libev 主循环)
```

**核心假设**: 少节点(3-5 台),固定拓扑,同机 Manager-Worker 走 shm,跨机走 TCP 直连。

---

## 2. 当前部署与 k8s 的冲突矩阵

| 现状 | k8s 上怎么处理 | 冲突 |
|------|---------------|------|
| `network_mode: host` + 127.0.0.1 绑定 | Pod 之间 127.0.0.1 不可达 | 必须改 |
| Manager↔Worker shm IPC | Pod 内能用,Pods 间不能用 | 同 Pod 则不变 |
| etcd `http://127.0.0.1:2379` | Service DNS 或 StatefulSet 地址 | 改 endpoint 配置 |
| node_id 去 etcd 槽位抢占 | StatefulSet 序号可替代 | 可保留 |
| S2S TCP 直连(IP:port) | ClusterIP Service 或 Pod IP | 需适配 |
| `io_uring` 直接操作 fd | k8s 不限制(容器有 CAP) | 无冲突 |
| WebSocket 长连接 | Service 负载均衡可能切断 | 需 SessionAffinity |

---

## 3. 分层决策

### 3.1 etcd——必须自建,不能蹭 k8s 的

```
k8s 自带的 etcd = kube-apiserver 独占, 是集群命根子
业务直连 k8s etcd → 反模式, 且托管 k8s(ACK/EKS/GKE)不暴露 etcd 端口
```

**方案 A(推荐): 独立 etcd StatefulSet**

```
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: thunder-etcd
spec:
  serviceName: thunder-etcd
  replicas: 3
  template:
    spec:
      containers:
      - name: etcd
        image: quay.io/coreos/etcd:v3.5.21
        env:
        - name: ETCD_NAME
          valueFrom: {fieldRef: {fieldPath: metadata.name}}
        - name: ETCD_INITIAL_CLUSTER
          value: "thunder-etcd-0=http://thunder-etcd-0:2380,thunder-etcd-1=..."
        volumeMounts:
        - name: data
          mountPath: /etcd-data
  volumeClaimTemplates:
  - metadata: {name: data}
    spec: {accessModes: [ReadWriteOnce], resources: {requests: {storage: 1Gi}}}
```

每个业务 Pod 配置 `etcd_endpoints: http://thunder-etcd-0.thunder-etcd:2379,http://thunder-etcd-1.thunder-etcd:2379,...`

### 3.2 网络——从 host 迁移到 ClusterIP

| 节点 | 端口 | k8s Service | 说明 |
|------|------|------------|------|
| Hello HTTP | 27006 | ClusterIP:27006 | 对外/对内 |
| Hello WS | 27010 | ClusterIP:27010 | websocket,需 `sessionAffinity:ClientIP` |
| Hello HTTPS | 27443 | ClusterIP:27443 | TLS 终结 |
| Interface | 27008 | ClusterIP:27008 | API 层 |
| Logic | 16068 | ClusterIP:16068 | 内部 S2S |

**Pod 配置去掉 `network_mode: host`, 监听 `0.0.0.0`(或 Pod IP)**

### 3.3 Manager↔Worker——同 Pod 不变

```yaml
# Logic Pod: Manager + Workers 同容器(和现在一样)
containers:
- name: logic
  image: thunder-logic
  command: ["./node.sh", "start"]
  # Manager fork Workers → shm IPC 正常工作(pod 内共享内存)
```

**不分拆 Pod**。拆了就得把 shm IPC 换成网络 IPC,得不偿失。

### 3.4 node_id——两条路

| 方式 | node_id 来源 | 重启变化 | 实现 |
|------|------------|---------|------|
| **A. 保持 etcd 槽位** (当前) | etcd slot 竞争 | lease 不过期不变 | 不改代码 |
| **B. StatefulSet 序号** | `${HOSTNAME##*-}` | 重启不变 | 去掉槽位逻辑,简化 connector |

**推荐 A(不改代码)**。B 更"k8s-native"但改了 connector 的注册核心。

---

## 4. 哪层适合 k8s,哪层不适合

```
第一层 — Hello/HelloWS(接入网关):
  长连接、低延迟、绑网卡、非 12-factor
  k8s overlay 网络多一跳(CNI 0.5~1ms)
  → 不太适合。裸机/VM 更好

第二层 — Logic(业务引擎):
  Manager+Worker 多进程, 同机 shm IPC
  放 k8s 上能跑, 但除了"统一运维"没额外收益
  → 可以做, 但没必要

第三层 — Interface(API 层):
  无状态 HTTP 接入, 会话在 redis
  最接近 12-factor
  → 最适合 k8s. 可以 HPA 弹性 + 滚动更新
```

---

## 5. 推荐路线

### 路线 1: 不动(当前)——最稳

```
3-5 台裸机/VM,固定拓扑,手工运维
Gateway → 裸机; Logic → 裸机; Interface → 裸机
好处: 性能最优,无网络 overlay,运维最简单
代价: 没弹性,没 k8s 生态
```

### 路线 2: 混合——务实折中

```
            ┌─────────────────────────┐
裸机/VM:    │ Hello(WS)  Logic        │ ← 性能敏感层,不动
            └─────────────────────────┘
                      ↑ S2S TCP
            ┌─────────────────────────┐
k8s:        │ Interface               │ ← 无状态 API 层,弹性 + CI/CD
            └─────────────────────────┘
```

**Interface 是唯一值得放 k8s 的层**——无状态、有弹性需求、不改核心链路。

### 路线 3: 全上 k8s——不推荐

```
需要解决:
  - etcd 独立 StatefulSet(可做)
  - 网络从 host→ClusterIP(可做,但延迟加 0.5~1ms)
  - WS 长连接需要 sessionAffinity(可做,但 k8s 负载均衡对这场景不是最优)
  - shm IPC 只能同 Pod(限制了扩展方式)
  - 滚动更新时 WebSocket 断连(需 graceful shutdown + drain)

结论: 技术上可以, 但"为了容器化而容器化", 性能倒退, 运维复杂度增加。
除非组织上强制"所有服务必须在 k8s 上", 否则不推荐。
```

---

## 6. 结论

| 问题 | 答案 |
|------|------|
| Thunder 能不能放 k8s | 能,但不应该全放 |
| 哪层最适合 k8s | **Interface**(无状态 API) |
| 哪层不应放 k8s | Hello/HelloWS(接入网关,对网络延迟敏感) |
| etcd 怎么部署 | 独立 StatefulSet,不蹭 k8s 自带的 |
| 全程最小改动方案 | etcd StatefulSet + 改 endpoint 配置 + 去掉 host 网络 → ClusterIP |
