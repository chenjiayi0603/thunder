# Thunder k8s 部署设计

> 日期: 2026-06-06
> 类型: 部署方案设计

---

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

## 7. 不推荐上 k8s 的服务

- **HelloWS** — WebSocket 长连接 + HPA scale-in 断连
- **Logic** — Manager+Worker 多进程 shm, k8s 没收益
- **DPDK backend** — 独占网卡, k8s 做不到

**结论**: Interface 最值得上 k8s(无状态+弹性), Logic/Hello 留裸机。
