# Thunder 工具集 — DPDK 逐层验证

## 策略

不在 Thunder 700 行里改。先写 150-300 行小例子，跑通一层再往 Thunder 搬。

```
Step 1: Ring PMD         → 验证 DPDK API 基线        ✅ 跑通
Step 2: AF_PACKET L2     → 验证旁路协议栈可行         ✅ 跑通
Step 3: F-Stack TCP Echo → 验证用户态 TCP 栈可行      ⏳ 等 F-Stack 安装
Step 4: 集成到 Thunder   → 搬进 DpdkIoBackend         ⏳ 待 Step 3 跑通
```

## 文件

| 文件 | 行 | 验证什么 | 依赖 |
|------|-----|---------|------|
| `dpdk_perf_final.c` | 100 | Ring PMD 虚拟端口收发 | DPDK |
| `dpdk_afpacket_echo.c` | 150 | AF_PACKET L2 帧收发 | DPDK |
| `dpdk_fstack_tcp_echo.c` | 277 | F-Stack TCP Echo (ff_socket/accept/read/write) | DPDK + F-Stack |

## 运行

```bash
# Step 1: Ring PMD
sudo ./tools/run_dpdk_perf_test.sh

# Step 2: AF_PACKET L2 Echo
sudo ./tools/run_dpdk_afpacket_echo.sh

# Step 3: F-Stack TCP Echo (需先装 F-Stack)
gcc -DHAVE_FSTACK -o /tmp/fstack_echo tools/dpdk_fstack_tcp_echo.c \
    -I/path/to/f-stack/lib/include -L/path/to/f-stack/lib -lfstack \
    [DPDK flags...]
sudo /tmp/fstack_echo --no-pci -l 0 -n 1 \
    --vdev=net_af_packet0,iface=lo \
    -d ${DPDK_PMD}/librte_mempool_ring.so \
    -d ${DPDK_PMD}/librte_net_af_packet.so
# curl http://127.0.0.1:9999 -d "hello"
```

## 性能层级 (5 种)

```
真DPDK PMD > Ring PMD > AF_PACKET > 原生socket > TAP PMD
  10-20x       ∞         3-5x        1x(基准)      0.5x
需专用网卡   纯内存     任何网卡     当前ev        调试用
```

详见 `docs/uring/DPDK+mTCP设计文档.md` 第 13 章。
