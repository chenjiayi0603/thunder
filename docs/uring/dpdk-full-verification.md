# Thunder DPDK 环境搭建与数据面验证

> **版本**: v1.0 | **日期**: 2026-05-19 | **DPDK**: 25.11

---

## 1. 概述

本文档记录在 Thunder 开发机上，**从零开始**搭建 DPDK 25.11 环境并完成数据面验证的全过程。

### 1.1 验证链路

```
零依赖 → 下载DPDK → 编译链接 → EAL初始化 → hugepages → mbuf pool → ring PMD → rx/tx burst → 数据校验
 ─────────────────────────────────────────────────────────────────────────────────────────────
  阶段1        阶段2       阶段3        阶段4        阶段5        阶段6        阶段7         阶段8
```

### 1.2 测试环境

| 项目 | 值 |
|------|-----|
| OS | Ubuntu 26.04 |
| Kernel | 7.0.0-15-generic |
| CPU | 20 cores, x86_64, 1 NUMA node |
| RAM | 30GB |
| 网卡 | Intel I219-V (e1000e) — **不直接用于 DPDK**，用 ring PMD 虚拟端口替代 |
| DPDK | 25.11，从 Ubuntu 软件源下载 .deb 提取到 `~/.local/dpdk/` |
| 测试方式 | ring PMD 虚拟端口 (net_ring0 ⇄ net_ring1)，用户态内存零拷贝转发 |

### 1.3 关键约束和解决方案

| 约束 | 解决方案 |
|------|---------|
| 无 DPDK 兼容物理网卡 (I219-V) | 使用 DPDK ring PMD — 纯软件虚拟端口，不需要任何物理硬件 |
| 无 root 权限安装系统包 | `apt download` 下载 .deb → `dpkg-deb -x` 提取到用户目录 |
| hugepages 需要 root | 一条 `sudo sysctl -w vm.nr_hugepages=256` |
| DPDK 25.11 shared library 插件机制 | 必须用 `-d` 参数运行时加载 `librte_mempool_ring.so` 和 `librte_net_ring.so` |

---

## 2. 原理：DPDK 数据面六层栈

在进入实操之前，先理解 DPDK 数据面的六个抽象层。每一层都是下一层的依赖，必须逐层验证。

```
┌─────────────────────────────────────────────────────────────┐
│ 6. 应用层     │ rx/tx burst → 收发数据包，业务逻辑            │
├─────────────────────────────────────────────────────────────┤
│ 5. 端口层     │ rte_eth_dev_* → 配置/启动/停止虚拟或物理端口   │
├─────────────────────────────────────────────────────────────┤
│ 4. 内存池层   │ rte_pktmbuf_pool_create → 分配/回收 mbuf     │
├─────────────────────────────────────────────────────────────┤
│ 3. 大页内存   │ hugepages → 2MB 连续物理页，DMA 友好          │
├─────────────────────────────────────────────────────────────┤
│ 2. EAL 层     │ rte_eal_init → CPU/内存/NUMA 抽象            │
├─────────────────────────────────────────────────────────────┤
│ 1. 编译链接层 │ gcc + -lrte_eal → 头文件/库文件正确链接        │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 为什么需要 hugepages？

| | 标准页 (4KB) | 大页 (2MB) | 差距 |
|---|---|---|---|
| 1GB 内存需要的页表条目 | 262,144 | 512 | **512x** |
| TLB 缓存命中率 | 低（频繁 miss） | 高（512x 更少条目） | — |
| 物理连续性 | 不保证 | 2MB 连续 | — |
| 网卡 DMA 友好 | 需要 IOMMU 映射 | 天然连续 | — |

DPDK 的 mbuf pool 是网卡 DMA 的目标内存。网卡硬件要求**物理连续**的内存地址。4KB 小页无法保证连续，而且页表遍历开销巨大。2MB 大页解决了这两个问题。

### 2.2 为什么 DPDK 25.11 必须显式加载插件？

DPDK 25.11 采用 **shared library + 插件** 架构。mempool 驱动和 PMD 驱动不再是编译进单一 `.a` 文件，而是独立的 `.so`：

```
~/.local/dpdk/usr/lib/x86_64-linux-gnu/
├── librte_eal.so          ← 运行时加载
├── librte_mempool.so      ← 框架（不含驱动实现）
├── librte_ethdev.so       ← 框架
└── dpdk/pmds-26.0/
    ├── librte_mempool_ring.so   ← 插件：ring 内存池驱动
    ├── librte_net_ring.so       ← 插件：ring 虚拟网卡
    ├── librte_net_af_packet.so  ← 插件：AF_PACKET 网卡
    └── ...
```

**陷阱**：仅用 gcc 链接 `-lrte_mempool -lrte_mempool_ring` 不够。驱动注册使用 `RTE_INIT` 构造函数宏，构造函数在 `.so` **加载**时执行。`-l` 链接只导入符号表，不触发 `dlopen` 式的构造器调用。必须通过 EAL 的 `-d` 参数在运行时显式加载：

```bash
./app -d /path/to/pmds-26.0/librte_mempool_ring.so
```

EAL 内部对每个 `-d` 参数调用 `dlopen()`，从而触发 `RTE_INIT` 构造函数，驱动才被注册到框架。

---

## 3. 阶段1-2：零依赖 → 下载 DPDK → 编译链接

### 3.1 下载（不需要 sudo）

```bash
# 下载 DPDK 25.11 及其所有依赖
mkdir -p /tmp/dpdk-pkgs && cd /tmp/dpdk-pkgs
apt download dpdk-dev libdpdk-dev librte-eal26 librte-ethdev26 \
              librte-mbuf26 librte-mempool26 librte-ring26 \
              librte-net26 librte-kvargs26 librte-log26 \
              librte-pci26 librte-bus-pci26 librte-bus-vdev26 \
              librte-hash26 librte-timer26 librte-rcu26 \
              librte-meter26 librte-mempool-ring26 \
              librte-net-ring26 libbsd-dev libbsd0 \
              libnuma-dev libnuma1 libfdt1 pkgconf-bin libpkgconf7

# 提取到用户目录
mkdir -p ~/.local/dpdk
for deb in *.deb; do dpkg-deb -x "$deb" ~/.local/dpdk; done
```

### 3.2 编译验证

```bash
DPDK_ROOT=~/.local/dpdk
DPDK_LIB=$DPDK_ROOT/usr/lib/x86_64-linux-gnu
DPDK_INC=$DPDK_ROOT/usr/include/dpdk
DPDK_ARCH=$DPDK_ROOT/usr/include/x86_64-linux-gnu/dpdk
DPDK_BSD=$DPDK_ROOT/usr/include/x86_64-linux-gnu

gcc -o /tmp/dpdk_hello hello.c \
    -include rte_config.h \               # 强制包含配置头
    -march=corei7 -mrtm -mssse3 \         # DPDK 必须的 SIMD 指令集
    -I$DPDK_INC -I$DPDK_ARCH -I$DPDK_BSD \
    -L$DPDK_LIB -Wl,-rpath,$DPDK_LIB \
    -lrte_eal -lrte_kvargs -lrte_log -lpthread -ldl
```

**为什么 `-march=corei7 -mrtm -mssse3`？**

DPDK 在 x86 上大量使用 SIMD 指令优化数据面：
- `-mssse3`：`_mm_alignr_epi8` 等内存拷贝原语
- `-mrtm`：TSX 事务内存（自旋锁优化）
- `-march=corei7`：AVX/SSE4.2 等现代 x86 特性

不加这些标志会编译失败（`error: '__builtin_ia32_palignr128' needs isa option -mssse3`）。

---

## 4. 阶段3-5：EAL 初始化 → hugepages → mbuf pool

### 4.1 hugepages 设置

```bash
# 分配 256 个 2MB 大页 = 512MB
sudo sysctl -w vm.nr_hugepages=256

# 挂载 hugetlbfs
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
sudo chmod 777 /dev/hugepages /mnt/huge
```

### 4.2 EAL 初始化验证

```bash
./dpdk_hello --no-pci -l 0 -n 1
# 输出:
# EAL: Detected CPU lcores: 20
# EAL: Detected NUMA nodes: 1
# EAL: Selected IOVA mode 'VA'
# dpdk_hello: EAL OK, 0 ports
```

**IOVA mode 'VA' 的含义**：DPDK 自动选择了 Virtual Address 模式作为 I/O 地址。这比 Physical Address (PA) 模式更灵活，不需要 root 权限的 `/proc/self/pagemap` 读取。

### 4.3 mbuf pool 创建（关键步骤）

```bash
# 必须用 -d 加载 mempool_ring 插件！
./dpdk_test --no-pci -l 0 \
    -d ~/.local/dpdk/usr/lib/x86_64-linux-gnu/dpdk/pmds-26.0/librte_mempool_ring.so
```

**代码**：
```c
struct rte_mempool *mp = rte_pktmbuf_pool_create("my_pool",
    4095,                          // 池大小（必须是 2^n - 1）
    0,                             // per-lcore 缓存 = 0
    0,                             // 私有数据大小
    2176,                          // 每个 mbuf 的数据空间 (RTE_MBUF_DEFAULT_BUF_SIZE)
    rte_socket_id());              // NUMA socket
// 返回非空 = 成功
```

**池大小的约束**：`n` 必须是 `2^k - 1`（如 255, 511, 1023, 2047, 4095），因为 ring buffer 的容量要求。

---

## 5. 阶段6-8：ring PMD → rx/tx burst → 数据校验

### 5.1 ring PMD 原理

ring PMD 是 DPDK 内置的纯软件虚拟网卡驱动。两个 ring 端口通过无锁 ring buffer 连接：

```
port0 (net_ring0)                    port1 (net_ring1)
    │                                     │
    │ rte_eth_tx_burst(0, ...)            │
    ├─ enqueue → ring buffer ──→ dequeue ─┤
    │                                     │ rte_eth_rx_burst(1, ...)
    │                                     │
    │ rte_eth_rx_burst(0, ...)            │
    ├─ dequeue ← ring buffer ←─ enqueue ─┤
    │                                     │ rte_eth_tx_burst(1, ...)
```

- **零拷贝**：数据在共享内存的 ring buffer 中传递，不经过任何拷贝
- **无中断**：纯轮询模式
- **不需要任何硬件**：完全在用户态内存中实现

### 5.2 完整验证代码

```c
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_version.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    // ① EAL 初始化
    rte_eal_init(argc, argv);
    
    // ② 创建 mbuf pool
    struct rte_mempool *mp = rte_pktmbuf_pool_create("p", 4095, 0, 0, 2176, 0);
    
    // ③ 配置两个 ring 端口
    for (uint16_t p = 0; p < 2; p++) {
        struct rte_eth_conf conf = {0};
        rte_eth_dev_configure(p, 1, 1, &conf);
        rte_eth_rx_queue_setup(p, 0, 128, 0, NULL, mp);
        rte_eth_tx_queue_setup(p, 0, 128, 0, NULL);
        rte_eth_dev_start(p);
    }
    
    // ④ 构造测试包
    const char *msg = "THUNDER_DPDK_DATA_PLANE_OK";
    struct rte_mbuf *tx = rte_pktmbuf_alloc(mp);
    char *data = rte_pktmbuf_append(tx, strlen(msg) + 1);
    memcpy(data, msg, strlen(msg) + 1);
    
    // ⑤ 发送 (port1 → port0)
    rte_eth_tx_burst(1, 0, &tx, 1);
    
    // ⑥ 接收 (port0)
    struct rte_mbuf *rx[32];
    uint16_t n = rte_eth_rx_burst(0, 0, rx, 32);
    
    // ⑦ 校验
    if (n > 0) {
        char *rx_data = rte_pktmbuf_mtod(rx[0], char*);
        printf("match=%d data='%s'\n",
            memcmp(rx_data, msg, strlen(msg)+1) == 0, rx_data);
        rte_pktmbuf_free(rx[0]);
    }
    
    // ⑧ 清理
    for (uint16_t p = 0; p < 2; p++) {
        rte_eth_dev_stop(p);
        rte_eth_dev_close(p);
    }
    rte_eal_cleanup();
    return 0;
}
```

### 5.3 运行

```bash
# 编译
gcc -o dpdk_full_test dpdk_full_test.c \
    -include rte_config.h -march=corei7 -mrtm -mssse3 \
    -I$DPDK_INC -I$DPDK_ARCH -I$DPDK_BSD \
    -L$DPDK_LIB -Wl,-rpath,$DPDK_LIB \
    -lrte_eal -lrte_ethdev -lrte_mbuf -lrte_mempool -lrte_ring \
    -lrte_net -lrte_hash -lrte_kvargs -lrte_log -lrte_pci \
    -lrte_bus_pci -lrte_bus_vdev -lrte_timer -lrte_rcu \
    -lpthread -ldl -lnuma

# 运行
sudo LD_LIBRARY_PATH=$DPDK_LIB ./dpdk_full_test \
    --no-pci -l 0 -n 1 \
    -d $DPDK_LIB/dpdk/pmds-26.0/librte_mempool_ring.so \
    -d $DPDK_LIB/dpdk/pmds-26.0/librte_net_ring.so \
    --vdev=net_ring0 --vdev=net_ring1
```

### 5.4 测试结果

```
DPDK DPDK 25.11.0 | ports=2 | hp=1
Pool: OK

TX 1→0: 'THUNDER_DPDK_DATA_PLANE_OK'
RX: 1 pkts
match=1 data='THUNDER_DPDK_DATA_PLANE_OK'

=== DPDK DATA PLANE OK ===
```

---

## 6. 运行要求分层

### 6.1 不需要 root 的测试

```
第1层 — 编译链接:
  gcc -lrte_eal ...           ← 验证头文件/库完整性（已完成 ✅）

第2层 — EAL 初始化:
  ./test --no-huge ...        ← 验证 CPU/NUMA 检测（已完成 ✅）

第3层 — 基础内存:
  rte_malloc / rte_memzone    ← 验证 hugepage 分配（已完成 ✅）
  rte_ring_create             ← 验证无锁队列（已完成 ✅）
```

### 6.2 需要 root 但不需要特殊硬件的测试

```
第4层 — 数据面:
  sudo sysctl vm.nr_hugepages=256      ← 一次性设置，可以写进 /etc/sysctl.conf
  ./test -d librte_mempool_ring.so     ← mbuf pool 创建
  ./test -d librte_net_ring.so         ← ring PMD 虚拟端口
  → rx/tx burst 双向收发              ← 完整数据面验证（已完成 ✅）

第5层 — 真网卡:
  sudo ./test -d librte_net_af_packet.so --vdev=net_af_packet0,iface=lo
  → AF_PACKET 真实网卡 I/O            ← 需要 CAP_NET_RAW（root）
```

### 6.3 需要 DPDK 兼容物理网卡的测试

```
第6层 — 硬件卸载:
  需要: Intel X710/82599, Mellanox CX4+, 或其他 DPDK 兼容 NIC
  sudo ./test --vdev=vfio 绑定物理网卡
  → 线速 10Gbps+ 吞吐测试
```

---

## 7. 与 Thunder 的集成路径

### 7.1 当前状态

```cpp
// DpdkIoBackend.cpp — 骨架（已编译通过）
// 所有方法有 //HAVE_DPDK 占位注释
bool DpdkIoBackend::SubmitRead(...) {
    // HAVE_DPDK: mtcp_read(mctx, sockid, buf->WriteBegin(), ...)
    (void)fd; return false;  // 骨架返回空
}
```

### 7.2 下一步：用 F-Stack 替换 mTCP

mTCP 不兼容 DPDK 25.11，改用 F-Stack。API 映射：

```
mTCP                    F-Stack                  DpdkIoBackend 方法
─────────────────────────────────────────────────────────────────
mtcp_socket()    →     ff_socket()              CreateListenSocket
mtcp_bind()      →     ff_bind()
mtcp_listen()    →     ff_listen()
mtcp_accept()    →     ff_accept()              Accept
mtcp_read()      →     ff_read()                HandleRead
mtcp_write()     →     ff_write()               HandleWrite
mtcp_close()     →     ff_close()               CloseFd
mtcp_epoll_wait  →     ff_epoll_wait()          ProcessMtcpEvents
```

### 7.3 最小集成验证

```bash
# 编译 F-Stack + Thunder
cmake -S . -B build_dpdk \
    -DTHUNDER_IO_DPDK=ON \
    -DFSTACK_ROOT=~/.local/fstack
cmake --build build_dpdk

# 配置 Thunder
echo '{"io_backend":"dpdk"}' > config.json
./build_dpdk/bin/Hello config.json

# 测试
curl http://localhost:8080/hello
```

---

## 8. 使用场景对比

### 8.1 什么时候用哪个后端

```
┌──────────────────────┬──────────┬──────────────┬──────────────┐
│ 场景                  │ ev       │ native_uring │ dpdk (F-Stack)│
├──────────────────────┼──────────┼──────────────┼──────────────┤
│ 开发/测试环境          │ ✅ 默认   │ ✅            │ ❌ 太重      │
│ 云服务器 (CVM/ECS)    │ ✅       │ ✅            │ ❌ 无 VFIO   │
│ 容器化 (K8s)          │ ✅       │ ✅            │ ❌ CNI 冲突  │
│ API 网关 (<C10K)     │ ✅       │ ✅            │ ⚠️ 杀鸡牛刀  │
│ WebSocket 长连接      │ ⚠️       │ ✅ 推荐        │ ⚠️           │
│ 游戏 TCP 网关 (C1M)  │ ❌       │ ⚠️            │ ✅ 推荐      │
│ 高 PPS (>5M/核)      │ ❌       │ ❌            │ ✅ 唯一选择  │
│ 微秒级延迟 (P99<50us)│ ❌       │ ⚠️            │ ✅            │
│ 物理机 + 兼容网卡     │ —       │ —             │ ✅ 必须      │
└──────────────────────┴──────────┴──────────────┴──────────────┘
```

### 8.2 DPDK 适用场景详解

**场景A: 游戏 TCP 接入网关**
- 百万级并发长连接 (C1M)
- 64B-256B 小包为主
- 需要 TCP 可靠传输
- 延迟敏感 (PVP: P99 < 10ms)
- **为什么 DPDK**：内核 TCP 栈在 C1M 连接时 TIME_WAIT 锁、fd 压力、内存开销剧增。用户态 TCP 每连接 <10KB，C10M 可行

**场景B: CDN 边缘节点**
- 线速 10Gbps+ 转发
- 大文件传输 (1500B MTU)
- 高吞吐是唯一指标
- **为什么 DPDK**：内核协议栈单核上限 ~5Gbps，DPDK 可达线速

**场景C: 流量清洗 / DPI**
- 每包处理 (>10M pps)
- 需要解析 L4-L7 协议
- 不需要完整 TCP 栈
- **为什么 DPDK**：零 syscall、零中断，CPU 100% 用于报文处理

### 8.3 什么时候不用 DPDK

| 场景 | 原因 |
|------|------|
| 云服务器 | 无物理网卡访问，DPDK 无法初始化 |
| K8s 容器化部署 | CNI 与 DPDK 冲突，需要 SR-IOV/Multus |
| 低频服务 (<1000连接) | DPDK 运维成本 > 性能收益 |
| 开发测试环境 | 无 DPDK 兼容网卡，无法验证 |
| 快速迭代业务 | 调试困难，无 strace/tcpdump |

---

## 9. 一键脚本

`scripts/setup_dpdk.sh` — 封装了所有初始化步骤：

```bash
./scripts/setup_dpdk.sh init    # 初始化 hugepages + 挂载 + memlock
./scripts/setup_dpdk.sh status  # 显示状态
./scripts/setup_dpdk.sh test    # 编译并运行 DPDK 数据面双向测试
./scripts/setup_dpdk.sh clean   # 释放 hugepages
```

---

## 10. 故障排查

| 错误 | 原因 | 解决 |
|------|------|------|
| `fatal error: rte_eal.h: No such file` | 缺少 `-I` 路径 | 添加 `-I$DPDK_INC -I$DPDK_ARCH` |
| `error: needs isa option -mssse3` | 缺少 SIMD 标志 | 添加 `-march=corei7 -mrtm -mssse3` |
| `fatal error: bsd/string.h: No such file` | 缺少 libbsd | `dpkg-deb -x libbsd-dev*.deb ~/.local/dpdk` |
| `Cannot allocate memory` (mbuf pool) | memlock 限制 | `sudo prlimit --memlock=unlimited` |
| `Invalid argument` (mbuf pool) | **插件未加载** | 添加 `-d librte_mempool_ring.so` |
| `failed to parse device "net_ring0"` | **插件未加载** | 添加 `-d librte_net_ring.so` |
| `open AF_PACKET socket: ...` | 需要 root | 用 `sudo` 运行 |
| `Not enough memory available` | hugepages 太少 | `sudo sysctl -w vm.nr_hugepages=256` |
| `Permission denied` (/dev/hugepages) | 用户无写权限 | `sudo chmod 777 /dev/hugepages` |

---

## 11. 参考

- DPDK 官方文档: https://doc.dpdk.org/guides/
- `docs/uring/dpdk-implementation-status.md` — 实现状态矩阵
- `docs/uring/DPDK+mTCP设计文档.md` — 原始架构设计
- `scripts/setup_dpdk.sh` — 一键初始化脚本
