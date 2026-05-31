# Thunder 工具集

## DPDK 数据面性能测试

### 文件

| 文件 | 说明 |
|------|------|
| `dpdk_perf_final.c` | DPDK ring 虚拟端口收发测试（功能/吞吐/延迟） |
| `run_dpdk_perf_test.sh` | 一键编译运行脚本 |

### 前置条件

当前环境 **不具备** 运行条件，需要：

1. **安装 DPDK**：
   ```bash
   # 安装到 ~/local/dpdk（脚本默认路径）
   # 或修改 run_dpdk_perf_test.sh 中的 DPDK_ROOT
   ```

2. **配置大页内存**：
   ```bash
   echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
   ```

3. **加载 DPDK 内核驱动**（如使用物理网卡）或使用 `--no-pci` 模式（仅 ring 虚拟端口，无需物理网卡）

### 编译运行

```bash
./tools/run_dpdk_perf_test.sh
```

脚本会：
- 用 gcc 编译 `dpdk_perf_final.c` → `/tmp/dpdk_perf_final`
- 通过 sudo 运行（DPDK 需要 root 权限初始化 EAL）

### 运行预期

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

Thunder 有 `DpdkIoBackend`（`code/Net/src/labor/DpdkIoBackend.cpp`），作为高性能 I/O 后端选型之一。此测试独立验证 DPDK 数据面是否可用——如果通过，说明 DPDK 环境 OK，可以切换 `io_backend` 配置启用 DPDK 模式。
