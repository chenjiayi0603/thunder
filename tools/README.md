# Thunder 工具集

## DPDK 数据面性能测试

### 文件

| 文件 | 说明 |
|------|------|
| `dpdk_perf_final.c` | DPDK ring 虚拟端口收发测试（功能/吞吐/延迟） |
| `run_dpdk_perf_test.sh` | 一键编译运行脚本 |

### 当前环境实测结果

```
$ ./tools/run_dpdk_perf_test.sh

EAL: Detected CPU lcores: 20
EAL: Detected NUMA nodes: 1
EAL: No free 2048 kB hugepages reported on node 0
EAL: Cannot get hugepage information.
RING: Cannot reserve memory for tailq
MEMPOOL: Cannot allocate tailq entry!
ETHDEV: Invalid port_id=0
Segmentation fault (core dumped)
```

✅ **编译通过** — gcc 链接成功  
❌ **运行失败** — DPDK EAL 初始化需要大页内存，当前 HugePages_Total=0

### 修复步骤

1. **配置大页内存**：
   ```bash
   echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
   ```

2. **重新运行**：
   ```bash
   ./tools/run_dpdk_perf_test.sh
   ```

### 运行预期（大页配置后）

```
Thunder DPDK 数据面性能验证
DPDK 26.0 | hp=1 | CPU=2400 MHz

0→1: OK (recv=1)
1→0: OK (recv=1)

吞吐量 (0→1, 1024B, burst=32):
  sent=10000 recv=10000 loss=0 Mbps=... PPS=...

延迟 (0→1 ping-pong, 128B):
  2000/2000 | min=...us avg=...us max=...us
```

### 与项目关系

Thunder 有 `DpdkIoBackend`（`code/Net/src/labor/DpdkIoBackend.cpp`），作为高性能 I/O 后端。此测试独立验证 DPDK 数据面——编译通过后，配好大页即可运行。
