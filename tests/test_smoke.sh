#!/bin/bash
# 冒烟测试 — 核心链路快速验证（需 Docker 集群已运行）
# 用法:
#   ./tests/test_smoke.sh              # 直接跑
#   ./tests/test_smoke.sh --build      # 先重编再跑
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

# ── 可选：先构建 ─────────────────────────────────────────────
if [ "${1:-}" = "--build" ]; then
    echo "=== 构建 ==="
    cmake --build build -j1 && cmake --install build
fi

# ── 检查集群是否已启动 ────────────────────────────────────────
if ! ss -tln 2>/dev/null | grep -q ':27006 '; then
    echo -e "${RED}✘ 集群未启动，请先执行 ./deploy.sh up${NC}"
    exit 1
fi

echo "=============================================="
echo "  Thunder 冒烟测试  $(date '+%Y-%m-%d %H:%M:%S')"
echo "=============================================="

# check <名称> <期望子串> <shell 命令>  — 失败计入 FAIL
check() {
    local name="$1" expect="$2" cmd="$3"
    local out
    out=$(eval "$cmd" 2>/dev/null) || true
    if echo "$out" | grep -q "$expect"; then
        echo -e "  ${GREEN}✅${NC} $name"
        PASS=$((PASS+1))
    else
        echo -e "  ${RED}❌${NC} $name  (got: ${out:0:100})"
        FAIL=$((FAIL+1))
    fi
}

# check_warn <名称> <期望子串> <shell 命令>  — 失败仅警告，不计入 FAIL
check_warn() {
    local name="$1" expect="$2" cmd="$3"
    local out
    out=$(eval "$cmd" 2>/dev/null) || true
    if echo "$out" | grep -q "$expect"; then
        echo -e "  ${GREEN}✅${NC} $name"
        PASS=$((PASS+1))
    else
        echo -e "  ${YELLOW}⚠️ ${NC} $name  (got: ${out:0:100})"
        SKIP=$((SKIP+1))
    fi
}

# ── HTTP / Hello ──────────────────────────────────────────────
echo ""
echo "--- HTTP / Hello ---"

check "HTTP Echo (POST)" \
    '"code":0' \
    'curl -sf --max-time 5 http://127.0.0.1:27006/hello/hello -d "{\"option\":\"Echo\"}"'

check "HTTP PoolCpu 协程挂起/恢复 (POST)" \
    '"checksum":786432' \
    'curl -sf --max-time 10 http://127.0.0.1:27006/hello/hello -d "{\"option\":\"TestHelloPoolCpu\"}"'

check "HTTPS Echo (POST)" \
    '"code":0' \
    'curl -skf --max-time 5 https://127.0.0.1:27443/hello/hello -d "{\"option\":\"Echo\"}"'

# WebSocket：-D - 把响应头输出到 stdout，不用 -f（101 非 2xx，-f 会失败）
check "WebSocket 握手 101" \
    '101 Switching Protocols' \
    'curl -s --max-time 3 -D - \
      -H "Upgrade: websocket" \
      -H "Connection: Upgrade" \
      -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
      -H "Sec-WebSocket-Version: 13" \
      http://127.0.0.1:27010/hello/shake'

# ── Interface → Logic 全链路 ──────────────────────────────────
echo ""
echo "--- Interface → Logic ---"

check "GenKey (POST)" \
    '"code":0' \
    'curl -sf --max-time 5 http://127.0.0.1:27008/Interface/gentoken -d "{\"option\":\"GenKey\"}"'

check "GenKey (GET + JSON body)" \
    '"code":0' \
    'curl -sf --max-time 5 -X GET \
      -H "Content-Type: application/json" \
      http://127.0.0.1:27008/Interface/gentoken \
      -d "{\"option\":\"GenKey\"}"'

check "VerifyKey 非法 token → code:1" \
    '"code":1' \
    'curl -sf --max-time 5 http://127.0.0.1:27008/Interface/gentoken \
      -d "{\"option\":\"VerifyKey\",\"token\":\"bad\",\"key\":\"bad\"}"'

# ── etcd ──────────────────────────────────────────────────────
echo ""
echo "--- etcd ---"

if curl -sf --max-time 3 http://127.0.0.1:2379/health >/dev/null 2>&1; then
    check "etcd health" \
        '"health":"true"' \
        'curl -sf --max-time 3 http://127.0.0.1:2379/health'

    # 节点用纯 lease keepalive（无 kv 写入），路由存内存
    # 验证：有活跃 lease（每个节点持有一个）表示节点在线
    check "etcd 节点 lease 活跃 (≥1)" \
        '"ID"' \
        'curl -sf --max-time 3 -X POST http://127.0.0.1:2379/v3/lease/leases \
          -H "Content-Type: application/json" -d "{}"'
else
    echo -e "  ${YELLOW}⚠️ ${NC} etcd :2379 不可达，跳过"
    SKIP=$((SKIP+1))
fi

# ── 汇总 ─────────────────────────────────────────────────────
echo ""
echo "=============================================="
TOTAL=$((PASS+FAIL+SKIP))
if [ "$FAIL" -eq 0 ]; then
    echo -e "  ${GREEN}全部通过 $PASS/$TOTAL${NC}"
    [ "$SKIP" -gt 0 ] && echo -e "  ${YELLOW}警告项 $SKIP${NC}（不计入失败）"
else
    echo -e "  ${RED}失败 $FAIL${NC} / 通过 ${GREEN}$PASS${NC} / 警告 ${YELLOW}$SKIP${NC} = $TOTAL"
fi
echo "=============================================="
exit $FAIL
