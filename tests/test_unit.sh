#!/bin/bash
# 单元测试 — C++ gtest + Python pytest
# 用法: ./test_unit.sh [--build]
set -e
THUNDER_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$THUNDER_ROOT"

if [ "${1:-}" = "--build" ]; then
  echo "=== 编译 ==="
  cmake --build build -j1
fi

echo "=== C++ 单元测试 ==="
ctest --test-dir build -j1 --output-on-failure 2>&1 | tail -5

echo "=== Python 单元测试 ==="
python3 -m pytest unit/ -v --tb=short 2>&1 | tail -20

echo "=== 完成 ==="
