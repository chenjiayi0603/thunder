#!/bin/bash
# Thunder 全量回归 (提交前必跑)
# 自动检测: 单元测试总是跑, E2E/冒烟需集群在运行
set -e
if [ "${1:-}" = "--build" ]; then
  echo "=== 构建 ==="; cmake --build build -j1; shift; fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
PASS=0; FAIL=0

green() { echo -e "\033[0;32m$*\033[0m"; }
red()   { echo -e "\033[0;31m$*\033[0m"; }

echo "============================================"
echo "  Thunder 全量回归"
echo "============================================"

# === 单元测试 (总是跑) ===
echo ""
echo "[1/3] C++ gtest (288 cases)"
if ctest --test-dir build/code/test -j1 --output-on-failure 2>&1 | grep -q "100% tests passed"; then
    green "  ✅ 全部通过"; PASS=$((PASS+1))
else
    red "  ❌ 有失败"; FAIL=$((FAIL+1))
fi

echo ""
echo "[2/3] Python pytest (60 cases)"
if python3 -m pytest tests/unit/ -q 2>&1 | grep -q "passed"; then
    green "  ✅ 全部通过"; PASS=$((PASS+1))
else
    red "  ❌ 有失败"; FAIL=$((FAIL+1))
fi

# === E2E + 冒烟 (需集群在运行) ===
echo ""
if ss -tln 2>/dev/null | grep -q ':27006 '; then
    echo "[3/3] Docker E2E + 冒烟"
    if python3 -m pytest tests/e2e/ -q --tb=short -m "integration or smoke" --mode=external 2>&1 | grep -q "passed"; then
        green "  ✅ E2E 全部通过"; PASS=$((PASS+1))
    else
        red "  ❌ E2E 有失败"; FAIL=$((FAIL+1))
    fi

    # 冒烟
    curl -sf http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}' | grep -q '"code":0' \
        && green "  ✅ HTTP Echo" || red "  ❌ HTTP Echo"
    curl -sf http://127.0.0.1:27006/hello/lua_node_type -d '{"mode":"fire_forget"}' | grep -q '"fire_forget_ok"' \
        && green "  ✅ Lua SendToNodeType fire-forget" || red "  ❌ Lua SendToNodeType fire-forget"
    curl -sf http://127.0.0.1:27008/Interface/gentoken -d '{"option":"GenKey"}' | grep -q '"code":0' \
        && green "  ✅ GenKey" || red "  ❌ GenKey"
    curl -sf http://127.0.0.1:26000/admin -d '{"cmd":"show","args":["center"]}' | grep -q "leader" \
        && green "  ✅ Center Raft" || red "  ❌ Center Raft"
else
    echo "[3/3] E2E + 冒烟 — 跳过 (集群未启动)"
    echo "  启动: docker compose -p thunder-test up -d"
fi

echo ""
echo "============================================"
echo "  完成: $PASS 通过"
[ $FAIL -gt 0 ] && echo "  $FAIL 失败 — 请检查上方输出"
echo "============================================"
