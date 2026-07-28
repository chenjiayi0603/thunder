# K8s 节点性能优化 — Node Tuner DaemonSet (#154)

> 版本: v1.0 | 日期: 2026-07-20 | 状态: 已部署验证

---

## 概述

Thunder 网关使用 `hostNetwork` 模式部署在 K8s 上，网络栈直接运行在宿主机内核上。为消除操作系统/K8s 默认配置引入的延迟抖动和吞吐瓶颈，通过 DaemonSet 在每个节点自动执行性能调优。

**优化维度**: CPU / 内存 / 网络 / 中断 / K8s 调度

---

## 优化清单

### 1. CPU Governor → performance ✅ 已实施

**背景**: Linux 默认 governor（`ondemand`/`powersave`）根据负载动态升降频。当流量突发时，CPU 从低频升到高频需要 20-50ms——这段时间内请求处理变慢，直接导致 P99 尾延迟尖刺。Thunder 网关需要稳定低延迟，不能接受这种不可预测的调速抖动。

**实现方式**: DaemonSet Init Container 写入 `/host/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`。对每个 CPU 核心逐个设置 `performance`，Main Container 每 300s 检查一次，发现漂移自动修复。

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

### 2. Transparent Hugepage → madvise ✅ 已实施

**背景**: 内核 Transparent Hugepage（THP）默认 `always` 时，后台定期扫描内存、将连续 4KB 页合并为 2MB 大页（compaction）。这个合并过程会短暂锁定内存页——如果恰好命中 Thunder Worker 正在读写的共享内存（路由表、ShmRingQueue），触发数十毫秒的停顿，表现为周期性延迟尖刺。改为 `madvise` 后，只有应用层显式调用 `madvise()` 时才合并，避免内核主动干扰。

**实现方式**: DaemonSet Init Container 写入 `/host/sys/kernel/mm/transparent_hugepage/enabled`。仅在当前值不是 `madvise` 时才操作，避免无意义的文件写入。

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

### 3. TCP/IP 协议栈优化 ✅ 已实施

**背景**: Thunder 使用 `hostNetwork` 直接共享宿主机 TCP 栈，内核默认参数为桌面/通用服务器场景设计，不适用于低延迟网关。关键问题：(1) `tcp_keepalive_time=7200s` 导致死连接 2 小时才释放，etcd/gRPC 长连接积压；(2) `tcp_slow_start_after_idle=1` 使空闲连接恢复传输时重新慢启动，连接池复用收益大打折扣；(3) `somaxconn=128` 在高并发突刺时直接丢 SYN。

**实现方式**: DaemonSet Init Container 通过 `sysctl -w` 逐项写入 `/host/proc/sys/net/`，并持久化到 `/host/etc/sysctl.d/99-thunder.conf`（节点重启后自动加载）。TCP buffer 通过 `sysctl net.ipv4.tcp_rmem/wmem` 直接写入。

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

### 4. 网卡 Ring Buffer ✅ 已实施

**背景**: NIC Ring Buffer 是网卡硬件和内核之间的 DMA 环形缓冲区。默认 256 个描述符在高吞吐（>1Gbps）时，内核来不及消费——Ring Buffer 满了，网卡只能丢包。Thunder 压测时 `netdev_backlog` 和 `ifconfig rx_missed` 持续增长即此现象。增大到 4096 后，内核有更大的缓冲窗口消化突发流量。

**实现方式**: DaemonSet Init Container 通过 `apk add ethtool` 后执行 `ethtool -G <iface> rx 4096 tx 4096`。自动检测物理网卡（有 `/sys/class/net/<iface>/device` 目录），取 `min(硬件上限, 4096)` 避免超出硬件能力。虚拟网卡跳过。

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

### 5. 网卡 IRQ Affinity ✅ 已实施

**背景**: 默认 `irqbalance` 将网卡中断分散到所有 CPU 核心，看似"负载均衡"，实则每个核心都可能被中断打断——正在执行请求处理的 Worker 线程频繁被 NIC 中断抢占，导致 CPU 缓存污染和尾延迟抖动。将 NIC IRQ 绑定到 CPU 0（专用 housekeeping 核心），业务 Pod 独占 CPU 1+，中断和业务物理隔离，互不干扰。

**实现方式**: DaemonSet Init Container 读取 `/host/proc/interrupts` 找到物理网卡的中断号，按核数策略写入 `/host/proc/irq/<N>/smp_affinity_list`：≥4 核 → CPU 0；2-3 核 → CPU 0-1；单核 → CPU 0。

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

### 6. kubelet CPU Manager → static ✅ 已实施

**背景**: K8s 默认 CFS 调度（`none` 策略）下，所有容器共享全部 CPU。即使 Pod 声明了 `requests=limits`，内核仍可能将不同 Pod 的线程调度到同一核心，频繁上下文切换导致 L1/L2/L3 缓存颠簸（cache thrashing）。启用 `static` 策略后，Guaranteed QoS Pod 独占指定 CPU——其他进程（包括 kubelet/systemd 的后台任务）不会抢占，缓存热度保持，延迟稳定。

**实现方式**: DaemonSet Init Container 修改 `/host/var/lib/kubelet/config.yaml` 中的 `cpuManagerPolicy: static`，并添加 `reservedSystemCPUs: 0` 为系统保留核心。修改后重启 kubelet 生效。Pod 侧需配合设置 `resources.requests.cpu == resources.limits.cpu`（整数核）以获得 Guaranteed QoS。

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

### 7. kubelet Topology Manager → best-effort ✅ 已实施

**背景**: 多 NUMA 节点服务器上，CPU 访问本节点内存 ~100ns，跨节点访问 ~150ns（+50%）。如果 Pod 的 CPU 分配在 NUMA 0 但内存分配在 NUMA 1，每次内存访问都走跨节点总线——高频场景下累积为可观测的吞吐下降。`best-effort` 策略尽量将 CPU/内存/设备对齐到同一 NUMA 节点，减少跨节点访问。选择 `best-effort` 而非 `single-numa-node` 是为了避免多 NUMA 节点上 Pod 因资源碎片无法调度。

**实现方式**: DaemonSet Init Container 修改 `/host/var/lib/kubelet/config.yaml` 中的 `topologyManagerPolicy: best-effort`。仅在检测到多 NUMA 节点（`numactl --hardware` 显示 >1 个 node）时才启用，单 NUMA 节点无需配置。

| 项目 | 说明 |
|------|------|
| **作用** | 尽量将 Pod 的 CPU/内存/设备分配在同一 NUMA 节点 |
| **策略** | `best-effort` — 尽力而为，不阻塞调度 (比 `single-numa-node` 更安全) |
| **生效条件** | 仅多 NUMA 节点 (>1) |

---

## 部署方式

> 以上 7 项优化的**实现途径**均为本节的 DaemonSet Init Container —— 通过一个 `alpine:3.20` 容器（~7MB），结合 `hostPID + privileged + hostPath(/)`，在 Pod 启动时一次性完成全部节点调优。

### DaemonSet 结构

> DaemonSet 是 K8s 控制器（不是容器），保证每个节点运行一个 Pod。当前未设 `nodeSelector`，覆盖**所有节点**（含 control-plane），靠 `tolerations: operator: Exists` 容忍 control-plane 污点 —— 单节点集群必需。

Pod 内包含两个容器，均使用 `alpine:3.20` 镜像：

| 容器 | 类型 | 镜像 | 生命周期 |
|------|:--:|------|:--:|
| `node-tuner` | Init Container | `alpine:3.20` (~7MB) | 一次性调优，跑完 8 步后 exit 0 |
| `watchdog` | Main Container | `alpine:3.20` (~7MB) | 驻留，每 300s 检查漂移，不退出 |

### 为什么用 DaemonSet + alpine:3.20

**DaemonSet** 保证每个节点跑一个 Pod，适合节点级操作（governor、sysctl、ethtool、IRQ 都是宿主机级别的，不是应用级）。节点扩缩容自动跟随——加新节点时 kubelet 自动调度 tuner Pod 上去初始化，不需要手动 SSH。比手动 sudo 更可靠：不会漏节点、幂等可重跑。

**alpine:3.20** 极简镜像仅 ~7MB（vs Thunder 业务镜像 ~100MB）。tuner 只需 shell 工具（`sh`、`cat`、`echo`、`sed`、`grep`、`nsenter`），不需要 C++ 运行时/protobuf/libev。`apk add ethtool` 按需安装，不预装。拉取快、启动快、无安全漏洞负担。

### 生命周期：Init Container 运行完就结束，Watchdog 一直运行

```
Pod 创建
  │
  ├─ Init Container (node-tuner)  ← 先跑这个
  │     │
  │     ├─ 幂等检查 → 已完成? → exit 0 (跳过)
  │     │
  │     ├─ 步骤 1-4: governor + THP + sysctl + NIC (无需重启，即时生效)
  │     │
  │     ├─ 步骤 5-6: kubelet CPU Manager + Topology Manager (改 config.yaml)
  │     │
  │     ├─ 配置有变更? → systemctl restart kubelet → 写 marker
  │     │     (kubelet 重启会杀死当前 Pod，Init Container 不会重跑——marker+boot_id 防重复)
  │     │
  │     └─ exit 0 ✅ Init Container 结束，不再运行
  │
  ├─ Main Container (watchdog)  ← Init Container 成功后才启动
  │     │
  │     └─ while true; sleep 300; 检查 drift; done  ← 一直运行，不退出
  │
  └─ Pod 删除 / 节点重启 → watchdog 被 kill → kubelet 重建 Pod → Init Container 重新检查
```

**关键点**：
- **Init Container 跑完即退出**（exit 0），不会持续占用资源。实际运行时间 <15s（无 kubelet 变更时）或 ~20s（含 kubelet 重启）。
- **Watchdog 一直运行**，但资源极省（`requests: cpu=10m, memory=16Mi`），每 300s 才醒来检查一次。
- 节点重启后 boot_id 变化 → Init Container 检测到与 marker 不匹配 → **重新执行全部步骤**。

### Init Container 执行步骤

**环境准备**：
- `hostPID: true` + `privileged: true` — 容器获得宿主机 root 权限
- `volumeMounts: /host → hostPath: /` — 宿主机根目录挂载到容器 `/host`
- `nsenter -t 1 -a` — 通过 PID 1 进入宿主机 namespace，操作宿主机 sysctl/ethtool/systemctl

**调优流程**（`k8s/node-tuner-daemonset.yaml`）：
```
Init Container 流程:
  0. 幂等检查 → 读 /host/run/thunder-node-init.done + 比对 boot_id
               同一次 boot 已完成 → skip; 节点重启后 boot_id 变化 → 重跑
  1. apk add ethtool → 安装 NIC 工具（网络不通则跳过，不影响后续）
  2. echo performance > /host/sys/.../cpu*/cpufreq/scaling_governor
  3. echo madvise > /host/sys/kernel/mm/transparent_hugepage/enabled + defrag
  4. 写 /host/etc/sysctl.d/99-thunder-performance.conf + nsenter sysctl -p
  5. ethtool -G <物理网卡> rx 4096 tx 4096 (取 min(硬件上限, 4096))
  6. NIC IRQ → /host/proc/irq/<N>/smp_affinity (≥4核→CPU0)
  7. 改 /host/var/lib/kubelet/config.yaml → cpuManagerPolicy: static
  8. 改 topologyManagerPolicy: best-effort (仅多 NUMA)
  9. 配置变更时 systemctl restart kubelet → 写 marker (含 boot_id)

Main Container (watchdog, 每 300s):
  检查 governor / THP / sysctl.somaxconn / sysctl.tcp_slow_start_after_idle / kubelet config
  发现漂移 → 自动修复
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
