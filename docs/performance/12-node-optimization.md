# K8s 节点性能优化 — Node Tuner DaemonSet (#154)

> 版本: v1.0 | 日期: 2026-07-20 | 状态: 已部署验证

---

## 概述

Thunder 网关使用 `hostNetwork` 模式部署在 K8s 上，网络栈直接运行在宿主机内核上。为消除操作系统/K8s 默认配置引入的延迟抖动和吞吐瓶颈，通过 DaemonSet 在每个节点自动执行性能调优。

**优化维度**: CPU / 内存 / 网络 / 中断 / K8s 调度

---

## 优化清单

### 1. CPU Governor → performance

| 项目 | 说明 |
|------|------|
| **默认值** | `ondemand` / `powersave` — CPU 空闲时降频 |
| **优化值** | `performance` — 恒定最高频率 |
| **收益** | 消除 CPU 升频延迟 (~20-50ms)，杜绝尾延迟尖刺 |
| **代价** | 功耗略增 (服务器场景可接受) |

```bash
# 验证
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
# → performance
```

### 2. Transparent Hugepage → madvise

| 项目 | 说明 |
|------|------|
| **默认值** | `always` — 内核主动合并 4KB 页为 2MB 大页 |
| **优化值** | `madvise` — 仅应用层显式请求时合并 |
| **收益** | 避免 THP compaction 引发的延迟抖动 (数十ms 级别) |
| **代价** | TLB miss 略增 (C++ 程序通常不依赖 THP) |

```bash
cat /sys/kernel/mm/transparent_hugepage/enabled
# → always [madvise] never
```

### 3. TCP/IP 协议栈优化

Thunder 网关使用 `hostNetwork`，直接共享宿主机 TCP 栈。以下优化针对 **低延迟 + 高并发 + 长连接** 场景：

| sysctl 参数 | 默认值 | 优化值 | 说明 |
|-------------|--------|--------|------|
| `tcp_keepalive_time` | 7200s | **60s** | 快速检测死连接 (对 etcd/后端 gRPC 长连接关键) |
| `tcp_keepalive_intvl` | 75s | **10s** | 探测间隔 |
| `tcp_keepalive_probes` | 9 | **6** | 探测次数 |
| `tcp_slow_start_after_idle` | 1 | **0** | 长连接空闲后禁用慢启动 (对连接池复用关键) |
| `tcp_tw_reuse` | 0 | **1** | TIME_WAIT 复用，减少高频建连端口耗尽 |
| `somaxconn` | 128 | **32768** | 连接队列上限 → 支持高并发突刺 |
| `tcp_max_syn_backlog` | 512 | **8192** | SYN 队列 |
| `netdev_max_backlog` | 1000 | **5000** | 网卡 → 内核协议栈队列 |
| `tcp_rmem` | 4096 87380 6MB | **4096 16384 128KB** | 接收 buffer: 低延迟优先 |
| `tcp_wmem` | 4096 16384 6MB | **4096 16384 128KB** | 发送 buffer: 低延迟优先 |

```bash
# 验证
sysctl net.ipv4.tcp_keepalive_time net.core.somaxconn net.ipv4.tcp_slow_start_after_idle
# → 60 / 32768 / 0
```

> ⚠️ TCP buffer 上限 128KB 适合小消息场景。若 Thunder 处理 >128KB 响应，需调大 (参考 [issues-list.md](../issues-list.md#3-tcp-buffer-大小需-profiling-验证))。

### 4. 网卡 Ring Buffer

| 项目 | 说明 |
|------|------|
| **默认值** | RX/TX 通常 256 (因厂商而异) |
| **优化值** | **4096** (min(硬件上限, 4096)) |
| **收益** | 增大 NIC DMA 环形缓冲区 → 减少高吞吐下丢包 |
| **实现** | `ethtool -G <iface> rx 4096 tx 4096` |

```bash
ethtool -g enp0s31f6 | grep -E "^RX:|^TX:"
# → RX: 4096, TX: 4096
```

### 5. 网卡 IRQ Affinity

| 项目 | 说明 |
|------|------|
| **默认值** | IRQ 分散到随机 CPU (irqbalance) 或固定单核 |
| **优化值** | NIC IRQ 绑定到 CPU 0 (housekeeping)，业务 Pod 使用 CPU 1+ |
| **收益** | 中断处理不打断业务线程 → 减少尾延迟；业务 CPU 无 IRQ 风暴干扰 |
| **策略** | ≥4 核: IRQ → CPU 0; 2-3 核: IRQ → CPU 0-1; 单核: IRQ → CPU 0 |

```bash
cat /proc/irq/193/smp_affinity_list
# → 0  (enp0s31f6 IRQ 已绑定到 CPU 0)
```

### 6. kubelet CPU Manager → static

| 项目 | 说明 |
|------|------|
| **默认值** | `none` — 所有容器共享全部 CPU (CFS 调度) |
| **优化值** | `static` — Guaranteed QoS Pod 独占指定 CPU 核心 |
| **前提条件** | Pod 必须设置 `requests == limits` (整数 CPU)，即 Guaranteed QoS |
| **收益** | 消除 CPU 缓存颠簸 (L1/L2/L3 cache thrashing)、减少 NUMA 跨节点访问 |

**当前绑核状态** (7 个网关 Pod):

| Pod | QoS | 独占 CPU |
|-----|-----|----------|
| thunder-hello | Guaranteed | **CPU 18** |
| thunder-hello-https | Guaranteed | **CPU 1** |
| thunder-hello-ws | Guaranteed | **CPU 19** |
| thunder-hello-wss | Guaranteed | **CPU 17** |
| thunder-interface | Guaranteed | **CPU 2-3** |
| thunder-logic | Guaranteed | **CPU 13-14** |
| thunder-logic-v2 | Guaranteed | **CPU 4-5** |

```bash
cat /var/lib/kubelet/cpu_manager_state | python3 -m json.tool
```

> ⚠️ `single-numa-node` 策略过于严格（可能导致 Pod Pending），当前使用 `best-effort`。

### 7. kubelet Topology Manager → best-effort

| 项目 | 说明 |
|------|------|
| **作用** | 尽量将 Pod 的 CPU/内存/设备分配在同一 NUMA 节点 |
| **策略** | `best-effort` — 尽力而为，不阻塞调度 (比 `single-numa-node` 更安全) |
| **生效条件** | 仅多 NUMA 节点 (>1) |

---

## 部署方式

### DaemonSet 自动部署

```bash
kubectl apply -f k8s/node-tuner-daemonset.yaml
```

**工作原理**:
```
┌─────────────────────────────────────────────────┐
│  Init Container (一次性, ~13s)                   │
│  1. apk install ethtool                          │
│  2. CPU governor → performance                   │
│  3. THP → madvise (仅当前不是时)                  │
│  4. sysctl 优化 + 持久化 (/etc/sysctl.d/)         │
│  5. NIC ring buffer → ethtool -G                 │
│  6. NIC IRQ affinity → /proc/irq/*/smp_affinity  │
│  7. kubelet CPU Manager → static                 │
│  8. kubelet Topology → best-effort (多NUMA)       │
│  9. kubelet restart (仅配置变更时)                │
│  10. 写入 marker (/host/run/thunder-node-init.done)│
├─────────────────────────────────────────────────┤
│  Main Container (Watchdog, 驻留)                 │
│  每 300s 检查: governor / THP / sysctl / kubelet │
│  发现漂移 → 自动修复                              │
└─────────────────────────────────────────────────┘
```

### 幂等设计

- **Marker 文件**: `/host/run/thunder-node-init.done` 包含 `boot_id`
- **同一次启动**: marker 存在 + boot_id 匹配 → 跳过 (幂等)
- **节点重启**: boot_id 变化 → 自动重新初始化
- **手动重跑**: `rm /host/run/thunder-node-init.done` + 重启 Pod

### 镜像选择

使用 `alpine:3.20` (~7MB)，非 `thunder-hello-http` (~100MB)。仅在需要 ethtool 的 Init Container 中 apk 安装。

---

## 验证方法

```bash
# 1. DaemonSet 状态
kubectl get pods -n thunder -l app=thunder-node-tuner

# 2. 查看初始化日志
kubectl logs -n thunder <pod> -c node-tuner

# 3. 检查各项优化
POD=$(kubectl get pods -n thunder -l app=thunder-node-tuner -o jsonpath='{.items[0].metadata.name}')

# Governor
kubectl exec $POD -- cat /host/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# THP
kubectl exec $POD -- cat /host/sys/kernel/mm/transparent_hugepage/enabled

# sysctl
kubectl exec $POD -- nsenter -t 1 -n -- sysctl net.ipv4.tcp_keepalive_time

# CPU Manager
kubectl exec $POD -- grep cpuManagerPolicy /host/var/lib/kubelet/config.yaml

# IRQ Affinity
kubectl exec $POD -- cat /host/proc/irq/<N>/smp_affinity_list

# Marker
kubectl exec $POD -- cat /host/run/thunder-node-init.done
```

---

## 注意事项

### ⚠️ 已知限制

1. **CPU 隔离未实现** — 系统进程 (kubelet/systemd) 仍可抢占业务核心
2. **TCP Fast Open** — Thunder C++ 代码未使用，sysctl 中已移除该配置
3. **TCP Buffer 128KB** — 适合小消息，大消息需 profiling 后调整
4. **IRQ affinity** — 仅对物理网卡生效 (有 `/sys/class/net/<iface>/device` 目录)，虚拟网卡跳过
5. **WiFi 网卡** — `ethtool -G` 可能失败 (硬件不支持)，属正常现象

### 🚫 不适用场景

- 非 `hostNetwork` 的 K8s Pod (网络栈在容器内，不共享宿主机 TCP 栈)
- Burstable/BestEffort QoS Pod (CPU Manager static 不生效)
- 单核节点 (IRQ affinity 无意义)

---

## 相关文件

| 文件 | 说明 |
|------|------|
| `k8s/node-tuner-daemonset.yaml` | DaemonSet 定义 |
| `k8s/*-deployment.yaml` | 各网关 Deployment (含 Guaranteed QoS 资源声明) |
| `docs/issues-list.md` | 未完成事项 |
