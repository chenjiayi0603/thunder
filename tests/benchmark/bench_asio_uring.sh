#!/bin/bash
# AsioUringIoBackend 性能基准 — IOPS + 吞吐对比 epoll
# 用法: THUNDER_IO_ASIO_URING=1 ./bench_asio_uring.sh
set -e
echo "=============================================="
echo "  AsioUringIoBackend 性能基准"
echo "  $(date '+%Y-%m-%d %H:%M:%S')"
echo "  $(uname -r) / $(nproc) cores"
echo "=============================================="

BIN=./build/bin/thunder_test_asio_uring
[ -x "$BIN" ] || { echo "先编译: cmake -DTHUNDER_IO_ASIO_URING=ON && cmake --build build --target thunder_test_asio_uring -j1"; exit 1; }

echo ""
echo "--- 单元测试 + 性能 ---"
$BIN --gtest_repeat=3 2>&1 | tail -10

echo ""
echo "--- IOPS (pipe write/read 循环) ---"
python3 -c "
import os, time
r, w = os.pipe()
buf = b'x' * 4096
n = 100000
t0 = time.time()
for _ in range(n):
    os.write(w, buf)
    os.read(r, 4096)
t1 = time.time()
iops = n / (t1 - t0)
print(f'pipe IOPS: {iops:,.0f} ops/s ({n} rounds, {t1-t0:.3f}s)')
os.close(r); os.close(w)
"

echo ""
echo "=============================================="
echo "  完成"  
echo "=============================================="
