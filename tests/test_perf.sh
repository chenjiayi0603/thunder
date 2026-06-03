#!/bin/bash
# 性能测试 — wrk 压测
# 用法: ./test_perf.sh [--build]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ "${1:-}" = "--build" ]; then
  echo "=== 构建 ==="
  cmake --build build -j1
  shift
fi

echo "=== 性能基准 ==="
./deploy.sh test bench --skip-build 2>&1 | tail -30

echo "=== 完成 ==="
