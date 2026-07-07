#!/bin/bash
# ShmRingQueue 性能基准 — 吞吐量 + 延迟对比
# 用法: ./bench_shm_queue.sh
set -e
BIN=./build/bin/thunder_test_shm_queue
[ -x "$BIN" ] || { echo "先编译: cmake --build build --target thunder_test_shm_queue -j1"; exit 1; }

echo "=============================================="
echo "  ShmRingQueue 性能基准"
echo "  $(date '+%Y-%m-%d %H:%M:%S')"
echo "  $(uname -r) / $(nproc) cores"
echo "=============================================="
echo ""

# 吞吐量: SPSC 1M ~ 10M msgs
for n in 1 5 10; do
    echo "--- Throughput ${n}M msgs ---"
    $BIN --gtest_filter="*Throughput_1M*" --gtest_repeat=3 2>&1 | grep "perf" | tail -2
    echo ""
done

# 延迟: 100K rounds × 3
echo "--- Latency 100K rounds ×3 ---"
$BIN --gtest_filter="*Latency*" --gtest_repeat=3 2>&1 | grep "perf" | tail -3
echo ""

echo "=============================================="
echo "  完成"
echo "=============================================="
