# Thunder 工具集

## DPDK 数据面性能测试

### 文件

| 文件 | 说明 |
|------|------|
| `dpdk_perf_final.c` | DPDK ring 虚拟端口收发测试（功能/吞吐/延迟） |
| `run_dpdk_perf_test.sh` | 一键编译运行脚本 |

### 前置条件

需要 sudo 分配大页内存：

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### 编译运行

```bash
./tools/run_dpdk_perf_test.sh
```

### 实测结果

```
Thunder DPDK 数据面性能验证
DPDK 25.11.0 | hp=1 | CPU=2918 MHz

0→1: OK (recv=1)
1→0: OK (recv=1)

吞吐量 (0→1, 1024B, burst=32):
  sent=10000 recv=10000 loss=0 Mbps=1222687 PPS=149253731

延迟 (0→1 ping-pong, 128B):
  2000/2000 | min=0us avg=0.0us max=1us
```

| 指标 | 结果 |
|------|------|
| 功能 | ✅ 双向收发 OK |
| 丢包 | 0 |
| 延迟 | min=0us, avg=0.0us, max=1us |
| 吞吐 (Mbps/PPS 为 ring 内存回环值，非真实网卡) | ~1222 Gbps / ~149M PPS |

### 与项目关系

Thunder 有 `DpdkIoBackend`（`code/Net/src/labor/DpdkIoBackend.cpp`），作为高性能 I/O 后端。此测试验证 DPDK 数据面可用——通过后可切换 `io_backend` 配置启用 DPDK 模式。
