#!/bin/bash
# 端到端测试 — Docker 集群 + etcd 回归
# 用法: ./test_e2e.sh [--build]
set -e
THUNDER_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$THUNDER_ROOT"

if [ "${1:-}" = "--build" ]; then
  echo "=== 编译 ==="
  cmake --build build -j1
fi

echo "=== Docker E2E ==="
../deploy.sh test e2e --skip-build 2>&1 | tail -20

echo "=== etcd 回归 ==="
bash e2e/test_etcd_regression.sh 2>&1 | tail -15

echo "=== 完成 ==="
