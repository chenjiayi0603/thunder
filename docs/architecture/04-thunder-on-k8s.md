# Thunder 在 K8s 上的部署架构

> 核心约束: Manager↔Worker 共享内存 IPC 只能在同 Pod 内；S2S 跨节点走 TCP 直连；etcd 自建

---

## 部署架构

```
┌──────────────── k8s Cluster ───────────────────┐
│                                                  │
│  ┌─ StatefulSet: thunder-etcd ──────────────┐  │
│  │  etcd-0 │ etcd-1 │ etcd-2                 │  │
│  │  (PVC)  │ (PVC)  │ (PVC)                  │  │
│  └───────────────────────────────────────────┘  │
│         ↑                                        │
│    etcd_endpoints: http://127.0.0.1:12379        │
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

## 网络模型

```
访问链路:
  Service → Pod → hostNetwork → 客户端

服务间调用 (S2S):
  所有 Pod 在同一宿主机，hostNetwork 共享宿主机 IP
  → Hello:27007 调 Logic:16068 = localhost 回环 = 零 CNI 开销
```

| 端口 | 服务 | 用途 |
|------|------|------|
| 16068 | Logic | S2S 内部调用 |
| 27007 | HelloHttp | HTTP 业务端口 |
| 27006 | HelloHttp | inner_port (S2S) |
| 27009 | Interface | 对外 REST API |
| 27011 | HelloWS | WebSocket |
| 27444 | HelloHttps | HTTPS |

---

## 服务 K8s 适应性

| 服务 | K8s 适应性 | 原因 |
|------|:--:|------|
| Interface | ✅ 可放 | 无状态 HTTP API，可 HPA，可滚动更新 |
| Hello (HTTP) | ✅ 可放 | 同 Interface，无状态 |
| Hello (WS/WSS) | ⚠️ 关 HPA | WebSocket 长连接怕断，固定副本 |
| Logic | ⚠️ 关 HPA | shm IPC 在同 Pod，固定副本 |
| DPDK | ❌ 不可 | 独占物理网卡，K8s 无法提供 |

### 自愈能力

```
Manager detect Worker 崩溃
  → RestartWorker (本地)
  → kill → fork new Worker → shm IPC 恢复

K8s detect Pod 崩溃
  → restart Pod
  → Container init → Manager → fork Workers
  → etcd lease: wait old lease expire → re-register
  → S2S: AutoConnect 自动重连
```

---

## etcd 在 K8s 中

```
StatefulSet: thunder-etcd (3 副本)
  Service: thunder-etcd (headless, ClusterIP=None)
    → DNS: thunder-etcd-0.thunder-etcd.default.svc.cluster.local:2379
    → DNS: thunder-etcd-1.thunder-etcd.default.svc.cluster.local:2379
    → DNS: thunder-etcd-2.thunder-etcd.default.svc.cluster.local:2379

Thunder 节点配置:
  "etcd_endpoints": "http://127.0.0.1:12379,http://127.0.0.1:12381,http://127.0.0.1:12383"
```
