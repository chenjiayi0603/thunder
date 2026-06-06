# Thunder k8s 部署方案

> 日期: 2026-06-06
> 包含: 适应性分析 + 部署设计 + 完整 YAML

---

## 1. 适应性分析



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

## 2. 部署设计



## 1. 概述

Thunder 当前在 docker-compose 以 `network_mode: host` 单机运行。迁移到 k8s 需要解决网络、etcd、服务发现三块。

**核心约束**:
- Manager↔Worker 共享内存 IPC 只能在同 Pod 内
- S2S 跨节点走 TCP 直连(不经过 CNI overlay 更高效)
- etcd 必须自建(不能蹭 k8s 控制面的 etcd)

---

## 2. 部署架构

```
┌──────────────── k8s Cluster ───────────────────┐
│                                                  │
│  ┌─ StatefulSet: thunder-etcd ──────────────┐  │
│  │  etcd-0 │ etcd-1 │ etcd-2                 │  │
│  │  (PVC)  │ (PVC)  │ (PVC)                  │  │
│  └───────────────────────────────────────────┘  │
│         ↑                                        │
│    etcd_endpoints: http://thunder-etcd-0:2379    │
│         │                                        │
│  ┌─ Deployment: thunder-logic ───────────────┐  │
│  │  Pod (2 容器):                             │  │
│  │    logic-manager:  Logic_robot (Manager)   │  │
│  │    logic-worker:    Logic_robot_W0 (Worker) │  │
│  │    shm: /dev/shm (IPC)                     │  │
│  └───────────────────────────────────────────┘  │
│                                                  │
│  ┌─ Deployment: thunder-interface ────────────┐  │
│  │  Service: ClusterIP:27008                  │  │
│  │  HPA: min=1, max=5 (CPU>70%)               │  │
│  └───────────────────────────────────────────┘  │
│                                                  │
│  ┌─ Deployment: thunder-hello ────────────────┐  │
│  │  Service: ClusterIP:27006                  │  │
│  │  固定副本(不建议 HPA, 长连接怕断)           │  │
│  └───────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘
```

---

## 3. etcd — StatefulSet

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: thunder-etcd
spec:
  serviceName: thunder-etcd
  replicas: 3
  selector:
    matchLabels:
      app: thunder-etcd
  template:
    metadata:
      labels:
        app: thunder-etcd
    spec:
      containers:
      - name: etcd
        image: quay.io/coreos/etcd:v3.5.21
        command:
        - etcd
        - --name=$(ETCD_NAME)
        - --data-dir=/etcd-data
        - --listen-client-urls=http://0.0.0.0:2379
        - --advertise-client-urls=http://$(ETCD_NAME).thunder-etcd:2379
        - --listen-peer-urls=http://0.0.0.0:2380
        - --initial-advertise-peer-urls=http://$(ETCD_NAME).thunder-etcd:2380
        - --initial-cluster=thunder-etcd-0=http://thunder-etcd-0.thunder-etcd:2380,thunder-etcd-1=http://thunder-etcd-1.thunder-etcd:2380,thunder-etcd-2=http://thunder-etcd-2.thunder-etcd:2380
        env:
        - name: ETCD_NAME
          valueFrom:
            fieldRef:
              fieldPath: metadata.name
        volumeMounts:
        - name: data
          mountPath: /etcd-data
  volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      accessModes: ["ReadWriteOnce"]
      resources:
        requests:
          storage: 1Gi
---
apiVersion: v1
kind: Service
metadata:
  name: thunder-etcd
spec:
  clusterIP: None  # Headless
  ports:
  - port: 2379
  - port: 2380
  selector:
    app: thunder-etcd
```

节点配置 etcd endpoint:
```
"etcd_endpoints": "http://thunder-etcd-0.thunder-etcd:2379,http://thunder-etcd-1.thunder-etcd:2379,http://thunder-etcd-2.thunder-etcd:2379"
```

---

## 4. 业务节点 — Deployment

### 4.1 Logic

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: thunder-logic
spec:
  replicas: 2
  selector:
    matchLabels:
      app: thunder-logic
  template:
    spec:
      containers:
      - name: logic
        image: thunder-logic:latest
        command: ["./node.sh", "start"]
        env:
        - name: ETCD_ENDPOINTS
          value: "http://thunder-etcd-0.thunder-etcd:2379,http://thunder-etcd-1.thunder-etcd:2379,http://thunder-etcd-2.thunder-etcd:2379"
        ports:
        - containerPort: 16068
```
**注意**: Manager+Worker 同 Pod,shm IPC 不变。不拆 Worker 出去——拆了就得换 gRPC,改了架构。

### 4.2 Interface — 可 HPA

```yaml
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: thunder-interface
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: thunder-interface
  minReplicas: 1
  maxReplicas: 5
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 70
```

Interface 无状态,天然适合弹性。

### 4.3 Hello — 不建议 HPA

Hello/HelloWS 有 WebSocket 长连接。HPA scale-in 直接断连。建议固定副本。

---

## 5. Service 暴露

```yaml
# Interface Service
apiVersion: v1
kind: Service
metadata:
  name: thunder-interface
spec:
  type: ClusterIP
  ports:
  - port: 27008
  selector:
    app: thunder-interface

# Hello HTTP (对外 → NodePort 或 Ingress)
apiVersion: v1
kind: Service
metadata:
  name: thunder-hello
spec:
  type: NodePort
  ports:
  - port: 27006
    nodePort: 30006
  selector:
    app: thunder-hello
```

---

## 6. 与 docker-compose 差异

| 项 | docker-compose | k8s |
|----|---------------|-----|
| 网络 | host 模式, 127.0.0.1 | ClusterIP Service, 0.0.0.0 监听 |
| etcd | 单节点, host 网络 | StatefulSet 3 节点, Headless Service |
| 配置 | 127.0.0.1:2379 | thunder-etcd-0.thunder-etcd:2379,... |
| S2S 路由 | etcd registry → IP:port 直连 | 同, 但 IP 是 Pod IP 或 Service ClusterIP |
| 端口 | 写死在 conf 里 | 写死在 conf 里, Service 映射 |

---

## 7. 哪些适合 k8s,哪些不适合

### Interface — ✅ 适合 k8s

Interface 只做一件事:收到 HTTP 请求(GenKey/VerifyKey),调 S2S 发给 Logic,把结果返回。每个请求独立,不依赖上一次请求的状态(session 在 Redis,不在本地内存)。所以:

- **挂了没事**: Pod A 挂了,流量自动切到 Pod B。新 Pod 启动后 etcd 注册,30 秒内加入路由表。
- **扩缩无状态**: 加 Pod 不需要迁移 session,减 Pod 没有断连问题(全是短连接 HTTP)。
- **滚动更新安全**: 新版本 Pod 和旧版本 Pod 可以同时存在。Client 无感知。

HPA 按 CPU 设置 70% 阈值:凌晨低峰自动减到 1 个 Pod,白天高峰自动加到 5 个。不需要人操作。

**为什么 Interface 可以开 HPA 而其他不行**: Interface 是纯 HTTP 短连接。HPA scale-in 杀 Pod → 在途请求失败 → 客户端自动重试 → Service 路由到另一个 Pod。一个请求重试一次,用户无感知。WebSocket 长连接断了就真断了——客户端没有自动重连机制。
HPA 按 CPU 设置 70% 阈值:凌晨低峰自动减到 1 个 Pod,白天高峰自动加到 5 个。不需要人操作。

### Hello — ⚠️ 勉强放 k8s

Hello 一个 Pod 里同时跑 HTTP(短连接)和 WebSocket(长连接)。关键区别:

- **HTTP 部分**: 同 Interface 一样无状态。放 k8s 完全没问题。
- **WebSocket 部分**: 客户端 ws:// → Hello, 连接建立后一直保持。问题有两个:

1. **HPA scale-in**: CPU 降到阈值以下 → k8s 删 Pod。Pod 被删 → 上面所有 WebSocket 连接断开。客户端没有"重新连到另一个 Pod"的机制——连到哪个 Pod 是 Service 路由决定的,客户端收到断连只能报错。

2. **滚动更新**: 新版本部署 → 旧 Pod 逐步销毁。旧 Pod 上的 WebSocket 连接全部断开 → 新 Pod 启动前有一段时间没有服务 → 如果客户端此时重连,可能连到还没准备好的新 Pod。

**能否解决**: 可以,但需要自己实现——添加 `preStop` hook (发 SIGTERM → 等排空 → 再退出),设置 `terminationGracePeriodSeconds: 60`,给 WebSocket 客户端加重连逻辑。这些都不是 k8s 自带的能力,要自己写。

**结论**: 如果 Hello 只跑 HTTP,放 k8s 没问题。如果跑 WebSocket,需要自己补优雅断开+重连,或者干脆不放 k8s。

### HelloWS — ⚠️ 关HPA可放

纯 WebSocket,每条连接都是长连接。问题比 Hello 更严重——Hello 至少还有 HTTP 部分可以正常扩缩,HelloWS 完全依赖长连接。

- HPA scale-in → 直接杀掉 Pod → 上面所有连接断开 → 没有"换个 Pod"这种概念,因为 Service 路由是随机的,断连后的重连可能到同一个旧 Pod,也可能到新 Pod——客户端不知道。
- 就算不用 HPA,固定 3 个副本:滚动更新时旧 Pod 销毁 → 连接断 → 新 Pod 启动 → 连接重建。如果 3 个 Pod 都同时更新,短暂时间内没有服务。

**k8s 可以跑长连接——关 HPA + 固定副本 + maxUnavailable=0 滚动更新即可。

### Logic — ⚠️ 关HPA可放

Logic 的架构有几个 k8s 无法获利的点:

**1. Manager+Worker shm IPC**

```
Manager 和 Worker 同进程用共享内存通信。
fork 后父子进程映射同一物理页 → 零拷贝。

k8s 上: 同 Pod 内可以继续用 shm。但 Manager 是单例(只该有一个),
Worker 可以多副本。如果把 Manager 和 Worker 拆成两个 Pod:
  shm 不能跨 Pod → 必须换 gRPC/TCP → 多了序列化+网络开销。
  原来 0 拷贝 7ns 延迟 → 变成网络调用 ~1ms 延迟。
```

**2. Manager 是单例**

```
集群里只需要一个 Logic Manager 管注册/路由/配置。
Worker 可以有多个。k8s 的 Deployment 把所有 Pod 一视同仁——
扩缩时不知道哪个是 Manager 哪个是 Worker。

方案: StatefulSet 固定 Pod-0 为 Manager,其他为 Worker。但 Manager 挂了
怎么选新 Leader? 需要自己实现选主——这正是旧 Center 做的事。
```

**3. S2S TCP 直连依赖固定 IP**

```
Interface 请求 Logic: 查 etcd registry → Logic ip:port → TCP 直连。
Pod IP 每次重启都变。k8s Service 可以提供固定 ClusterIP,但:
  ClusterIP → iptables NAT → 多一跳。
  直连 Pod IP → 下次重启变了 → 路由表过期 → 需要 watch etcd 更新。
```

**结论**: Logic 放 k8s 同 Pod 跑就可以, 不需要拆。关 HPA, 固定 replicas。和 docker-compose 一样。

### DPDK — ⚠️ 关HPA可放

DPDK 的 PMD 驱动接管物理网卡——用户态直接操作 DMA ring,跳过内核协议栈。k8s 上:

- `hostNetwork: true` 能让 Pod 看到宿主机网卡。但**多个 Pod 不能同时 bind 同一网卡**——DPDK 没有虚拟化。
- 即使一个节点只跑一个 DPDK Pod:Pod 调度到哪个节点不可控,且网卡需要 DPDK 兼容 NIC(Intel X520/X710 等),k8s 节点可能有混插的。
- DPDK 场景天然适合裸机:固定物理机,网卡配置一次不变,追求极致性能。

### 结论

```
Interface (API网关) → ✅ 可放 k8s  无状态 API, 可 HPA, 可滚动更新
Hello    → 关 HPA 可放 k8s
Logic    → ✅ 可放 (关 HPA)
Hello    → ✅ 可放 (关 HPA)
HelloWS  → ✅ 可放 (关 HPA)
DPDK     → 裸机/VM ❌  独占网卡
```


---

## 最终结论

所有 Thunder 节点都支持内部重连机制,杀 Pod 后自动恢复,无需外部干预。

| 服务 | 可放 k8s | 原因 |
|------|---------|------|
| Interface (API网关) | ✅ | 内部 S2S 自愈, etcd路由自动更新 |
| Logic | ✅ | Manager+Worker 同Pod, shm正常, etcd重注册 |
| Hello | ✅ | 同上, 外部WS重连由客户端处理 |
| HelloWS | ✅ | 同上 |

**无需纠结 HPA 开不开** — 那是业务层决策,不影响能放 k8s。


---

## 注意事项

### Gateway (Hello/HelloWS/Interface) — 对外 IP 不要变

客户端连的是 IP:port。Pod IP 每次重启都变 → 客户端连不上。

| 方案 | 说明 |
|------|------|
| NodePort | 固定宿主机 IP:NodePort, Pod 调度到哪个节点都能路由 |
| LoadBalancer | 云厂商 LB → 固定外网 IP |
| Ingress | HTTP 路由, 但对 WS 需要额外配置 |

**不要用 ClusterIP 暴露给外部** — 外部客户端访问不到集群内部 IP。

### etcd — 3 节点不要变

etcd 是 Raft 集群(3 节点)。**副本数写死 3**,不要改 StatefulSet replicas。加了新节点需要手动 ,删节点需要 ——不是 k8s 自动做的。

**不要共用 etcd 数据目录**。每个 Pod 有独立 PVC,数据在 。

### Logic — Manager 被杀代价大

Manager 是单例。被 HPA 或节点驱逐杀掉 → 所有 Worker 一起死 → 相当于整个 Logic 服务重启。虽然会自动恢复,但恢复期间(2~5s)不可用。建议:

-  做冗余(两个 Pod,一个是备用)
- 不用 HPA
- : 两个 Pod 不在同一节点


---

## 8. 暴露方式 (NodePort vs Ingress)

Interface/Hello/HelloWS 三个服务都直接对客户端。暴露方式不影响服务能力,只是运维配置。

**NodePort** — 简单, 每个服务一个端口:


**Ingress (nginx)** — 将多个服务合并到一个入口:


**Thunder 不需要 Ingress**。docker-compose 现在三个端口直接暴露, k8s 也沿用。Ingress 是 k8s 运维的可选项,不是 Thunder 的要求。
  

## 9. 域名与暴露

客户端通过域名访问, 不走 nginx/Ingress 额外代理层:



**不需要 Ingress**。DNS 指向 Node IP, NodePort 直接到 Pod, 零额外跳转。和 docker-compose 一样, 只是 k8s 管 Pod 生命周期。


---

## 10. 为什么用 etcd 而不是 CoreDNS

k8s 原生的服务发现是 CoreDNS (service-name.namespace.svc → ClusterIP)。Thunder 不用它:



**优势**:
1. **零 DNS 依赖** — 不需要维护 Service/ClusterIP/DNS 记录
2. **跨环境一致** — docker-compose 和 k8s 用同一套 etcd 注册逻辑
3. **直连低延迟** — 不经过 kube-proxy iptables 跳转
4. **混部友好** — k8s 上的 Interface 可以直接连裸机上的 Logic(只要 etcd 可达)

**etcd 仍然需要 Service** — 仅用于 etcd 本身的发现(StatefulSet Headless Service: )。业务节点之间的发现走 etcd registry,不走 CoreDNS。
