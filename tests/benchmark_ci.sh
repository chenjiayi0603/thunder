#!/bin/bash
# 性能基准测试 (本机运行)
set -euo pipefail
BENCH_DIR="build/code/test"
echo "=== Work-Stealing Pool ===" && $BENCH_DIR/thunder_bench_work_stealing --gtest_filter='BenchWorkStealing.Compare' 2>&1 | grep -E '1P-4C.*ns|WS.*ns'
echo "=== ThreadPool Queue ===" && $BENCH_DIR/thunder_bench_threadpool_queue --gtest_filter='BenchThreadpoolQueue.Compare' 2>&1 | grep -E '4P-4C.*ns|LF.*ns'
echo "=== Queue Latency ===" && $BENCH_DIR/thunder_bench_queue_latency --gtest_filter='BenchQueueLatency.*' 2>&1 | grep -E '4.*worker.*ns|WS.*ns'
