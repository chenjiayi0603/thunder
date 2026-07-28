# Thunder 网关部署方案

> 面向运维人员：单机 / 多物理机 / K8s / 云上的 Thunder 网关部署选型。

---

## 1. 单机部署

Thunder Worker 直接监听端口，客户端直连。

```
客户端 → Thunder Worker (0.0.0.0:27006)
```

优点：零中间层，最低延迟。适合开发、测试、单机小规模生产。

---

## 2. 多物理机部署

### 2.1 DNS 轮询（推荐，最简单）

```
客户端 → DNS 解析 thunder.example.com
              ├→ 192.168.1.10:27006 (Thunder-1)
              ├→ 192.168.1.11:27006 (Thunder-2)
              └→ 192.168.1.12:27006 (Thunder-3)
```

一个域名解析出多个 A 记录，客户端取第一个或随机选。全节点同时工作，零中间层。

| 优点 | 缺点 |
|------|------|
| 零额外延迟，无中间层 | 节点挂了客户端可能继续请求（DNS 缓存） |
| Thunder 直连客户端 | 长连接建立后不迁移 |
| 配置最简单 | 无健康检查 |

**适用**：短连接为主、3~5 台物理机、接受 DNS TTL 级别的故障切换延迟。

### 2.2 keepalived VIP（双机热备）

```
客户端 → 192.168.1.100 (VIP)
              │
         ┌────┴────┐
    Thunder-1    Thunder-2
    (master)     (backup)
```

keepalived 通过 VRRP 协议维护一个虚拟 IP（VIP），只在 master 上生效。master 挂了 VIP 漂移到 backup。

| 优点 | 缺点 |
|------|------|
| 无额外延迟（客户端直连） | 只有一台干活，backup 闲置 |
| 故障切换快（秒级） | 总吞吐等于单机上限 |
| 配置简单 | 仅适合两台机器 |

**适用**：两台机器、追求简单可靠、不需要多机并行的高吞吐场景。

### 2.3 LVS + keepalived（多机，但增加复杂度）

```
客户端 → keepalived VIP → LVS (DR 模式，主/备)
                              │
                   ┌──────────┼──────────┐
              Thunder-1   Thunder-2   Thunder-3
```

LVS 做四层转发，所有 Thunder 同时工作。DR 模式下 Thunder 回包直连客户端（不经 LVS）。

```bash
# LVS 配置示例
ipvsadm -A -t VIP:27006 -s rr
ipvsadm -a -t VIP:27006 -r 192.168.1.10 -g
ipvsadm -a -t VIP:27006 -r 192.168.1.11 -g
ipvsadm -a -t VIP:27006 -r 192.168.1.12 -g
```

| 优点 | 缺点 |
|------|------|
| 多机并行，吞吐接近线性扩展 | 多两层（keepalived + LVS） |
| 支持健康检查 | LVS 自身需高可用 |
| — | 增加运维复杂度 |

**适用**：三台以上、需要健康检查、接受中间层开销。

### 2.4 方案对比

| | DNS 轮询 | keepalived | LVS |
|------|:--:|:--:|:--:|
| 并发工作节点 | 全部 | 1 台 | 全部 |
| 中间层 | 无 | 无 | LVS + keepalived |
| 故障切换速度 | DNS TTL（分钟级） | VRRP（秒级） | 秒级 |
| 健康检查 | 无 | 有 | 有 |
| 配置复杂度 | 最低 | 低 | 高 |
| 长连接负载均衡 | 仅新连接 | 仅新连接 | 仅新连接 |

> **长连接无法在物理机之间迁移。** TCP 连接绑定四元组 (src_ip, src_port, dst_ip, dst_port)，一旦建立只能等断开。负载均衡仅对新连接生效。长连接场景靠连接池 + 客户端重连实现切换。

---

## 3. 优雅重启的范围

Thunder 的优雅重启（GracefulRestart）是**单机内部**的：Manager 收到 SIGTERM 后，排空当前 Worker 的在途连接，然后重启 Worker 进程。

```
一台物理机内部:
  Manager → SIGTERM → 排空连接 → 重启 Worker

不涉及:
  ❌ 跨物理机连接迁移
  ❌ 故障转移
  ❌ 长连接负载均衡
```

---

## 4. K8s 部署

### 4.1 NodePort（自建集群 / kubeadm）

```yaml
# hello-deployment.yaml
spec:
  type: NodePort
  ports:
    - port: 27006
      nodePort: 30006
```

```
客户端 → Node IP:30006 → Thunder Pod:27006
```

所有 Thunder Pod 同时工作，K8s 的 Service 提供 Pod 级别的负载均衡（iptables/IPVS）。

### 4.2 hostNetwork（追求极致性能）

```
客户端 → 宿主机 IP:27006 → Thunder Pod (直接绑宿主机网卡)
```

跳过所有 K8s 网络层（CNI、kube-proxy），延迟最低。代价是端口不能冲突，调度不灵活。

### 4.3 NodePort vs hostNetwork — Thunder 网关适配性

Thunder 作为高性能网关（HTTP 64B 延迟 220μs），K8s 网络层的额外开销不可忽略。

```
hostNetwork:
  网卡 → 内核协议栈 → Thunder Pod (直通，零额外跳)
  延迟: 220μs (Thunder 裸性能)

NodePort:
  网卡 → 内核 iptables/IPVS 规则匹配 → CNI 虚拟网卡 → Thunder Pod
  延迟: 220μs + 网络层开销 (~几~几十 μs)
```

| | hostNetwork | NodePort |
|------|:--:|:--:|
| 延迟 | Thunder 裸性能 | 多 ~几~几十 μs (iptables/IPVS + CNI) |
| 端口 | 只能用 27006/27443/27010 等固定端口 | 可用 30006/30043 等映射端口，避免冲突 |
| 调度 | Pod 绑定宿主机 IP，调度不灵活 | Pod 可调度到任意节点 |
| pod 重启 | IP 不变（宿主机 IP） | IP 不变（Service 映射） |
| 多实例同机 | ❌ 端口冲突 | ✅ 不同 nodePort |
| 安全隔离 | 无（共享宿主机网络） | 有（CNI 网络隔离） |

**对 Thunder 的推荐**：

| 场景 | 推荐 | 前提 |
|------|:--:|------|
| 性能基准测试 | hostNetwork | — |
| 独占机器（物理机/云 VM，只跑 Thunder） | hostNetwork | 端口不冲突，零网络层开销 |
| 与其他服务共享节点 | NodePort | 同一节点上多个 Pod，需要端口隔离 |
| Pod 可能漂移到任意节点 | NodePort + 云 LB | 需要固定入口（云 LB）或 Service 映射 |

> 核心原则：**只要端口不冲突就用 hostNetwork**。Thunder 的高性能在 hostNetwork 下才能完全释放。选择 NodePort 时确保 kube-proxy 使用 IPVS 模式。云 LB 仅提供固定入口，不替代 Thunder 的网关功能——不需要 Ingress Controller。

### 4.4 云上（AWS/GCP/Azure）

```
客户端 → 云 LB (80/443) → NodePort (30006/30043) → Thunder Pod
```

云 LB 提供固定入口 IP + 健康检查。Thunder 自己就是网关——不需要 Ingress Controller。TLS 可以在 Thunder 侧终结，也可以在云 LB 侧终结（取决于是否需要 Thunder 拿到原始请求 IP）。

### 4.4 不需要的组件

| 组件 | 为什么不需要 |
|------|------------|
| Ingress Controller (nginx-ingress/traefik) | Thunder 就是网关，比 nginx 还快，前面加一层等于降速 |
| LoadBalancer Service (裸金属) | 永远 Pending，没有云 API |
| Service Mesh (Istio/Linkerd) | etcd 已覆盖服务发现和配置 |

---

## 5. TLS 终结位置

| 位置 | 优点 | 缺点 |
|------|------|------|
| **Thunder 侧** | 原始 IP 可获取，Thunder 全权控制 | 消耗 Thunder 的 CPU |
| 云 LB 侧 | Thunder 不消耗 SSL-CPU | 回源 HTTP 明文传输；Thunder 拿不到客户端 IP（需 X-Forwarded-For） |

> 推荐 Thunder 侧终结——Thunder 的 asio_uring 在 HTTPS 场景下延迟比 Nginx 低 2~3 倍，没必要把 SSL 卸给 LB。

---

## 6. 推荐方案

| 场景 | 推荐 | 理由 |
|------|:--:|------|
| 单机开发/测试 | 直连 Thunder | 最简单 |
| 2 台物理机 | keepalived VIP | 简单可靠，无中间层 |
| 3~5 台物理机 | DNS 轮询 | 零中间层，全节点并行 |
| 5 台以上 | DNS 轮询 + 健康检查监控 | 需额外脚本摘故障节点 DNS |
| kubeadm 自建 K8s | NodePort 或 hostNetwork | 无云 LB，直接暴露 |
| 云上 K8s | 云 LB → NodePort → Thunder | LB 仅固定入口，Thunder 做所有网关工作 |

---

## 附录 A：K8s 网络层对高性能网关的开销分析

### A.1 数据路径对比

```
hostNetwork Pod:
  网卡 → 宿主机内核协议栈 → Pod 进程 (直通, 零中间跳)
  数据面经过: 0 个 K8s 组件

NodePort Pod:
  网卡 → 宿主机内核 → iptables/IPVS 规则 → CNI veth pair → Pod 网络 namespace → Pod 进程
  数据面经过: kube-proxy 规则 + CNI 虚拟网卡
```

### A.2 哪些影响数据面延迟

| 组件 | hostNetwork 下 | NodePort 下 | 性质 |
|------|:--:|:--:|------|
| kube-proxy (iptables/IPVS) | 无 | 有，每次包匹配规则 | 数据面 |
| CNI (flannel/calico) | 无 | 有，veth pair 桥接转发 | 数据面 |
| containerd | 无影响 | 无影响 | **仅在控制面** |
| kubelet / API Server | 无影响 | 无影响 | 控制面 |
| cgroup 资源限制 | 可能 CPU throttle | 可能 CPU throttle | 控制面（仅限流时） |

### A.3 containerd 为什么不影响网络延迟

containerd 只做 Pod 生命周期管理：拉镜像、创建 namespace、启动进程。Pod 启动后网络流量完全不经 containerd。hostNetwork 下 Pod 直接继承宿主机网络 namespace，数据包走的是内核协议栈直达进程。

### A.4 hostNetwork 下仍存在的控制面开销

| 开销 | 影响 |
|------|------|
| Pod 调度延迟 | 新 Pod Pending → Running 需几秒 |
| kubelet 心跳 | 每 10s 向 API Server 报告，不影响数据面 |
| etcd 压力 | 集群规模越大，控制面延迟越高 |
| cgroup CPU limit | 达到上限时强制 throttle（P99 可能恶化） |

> 结论：hostNetwork 下数据面开销为零，所有潜在影响来自控制面。对 Thunder 的 220μs HTTP 延迟没有可测量影响。
