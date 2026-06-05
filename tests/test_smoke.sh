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

check "Redis set/get (CoRedis)" \
    '"set_ok":1' \
    'curl -sf --max-time 5 http://127.0.0.1:27006/hello/hello -d "{\"option\":\"TestHelloCoRedis\"}"'

check "MySQL create/insert/select (CoMysql)" \
    '"select_ok":1' \
    'curl -sf --max-time 5 http://127.0.0.1:27006/hello/hello -d "{\"option\":\"TestHelloCoMysql\"}"'

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

# 请求 1：GenKey 生成 token（POST）
_GENKEY_POST=$(curl -sf --max-time 5 http://127.0.0.1:27008/Interface/gentoken \
  -d '{"option":"GenKey"}' 2>/dev/null)
check "GenKey (POST)" '"code":0' "echo '${_GENKEY_POST}'"

# 请求 2：VerifyKey 验证上一步拿到的 token
_TOKEN=$(echo "$_GENKEY_POST" | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
_KEY=$(echo   "$_GENKEY_POST" | python3 -c "import sys,json; print(json.load(sys.stdin).get('key',''))"   2>/dev/null)
check "VerifyKey 有效 token → code:0" \
    '"code":0' \
    "curl -sf --max-time 5 http://127.0.0.1:27008/Interface/gentoken \
      -d '{\"option\":\"VerifyKey\",\"token\":\"${_TOKEN}\",\"key\":\"${_KEY}\"}'"

# 请求 3：GenKey（GET + JSON body）
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

check "VerifyKey 空 token → code:1" \
    '"code":1' \
    'curl -s --max-time 5 http://127.0.0.1:27008/Interface/gentoken \
      -d "{\"option\":\"VerifyKey\",\"token\":\"\",\"key\":\"\"}"'

# ── etcd ──────────────────────────────────────────────────────
echo ""
echo "--- etcd ---"

if ! curl -sf --max-time 3 http://127.0.0.1:2379/health >/dev/null 2>&1; then
    echo -e "  ${YELLOW}⚠️ ${NC} etcd :2379 不可达，跳过 etcd 段"
    SKIP=$((SKIP+1))
else
    _KEY=$(python3 -c "import base64; print(base64.b64encode(b'/thunder/registry/').decode())")
    _END=$(python3 -c "import base64; print(base64.b64encode(b'/thunder/registry0').decode())")
    _REG=$(curl -sf --max-time 3 http://127.0.0.1:2379/v3/kv/range \
        -d "{\"key\":\"$_KEY\",\"range_end\":\"$_END\"}" 2>/dev/null)
    _SKEY=$(python3 -c "import base64; print(base64.b64encode(b'/thunder/slot/').decode())")
    _SEND=$(python3 -c "import base64; print(base64.b64encode(b'/thunder/slot0').decode())")
    _SLOT=$(curl -sf --max-time 3 http://127.0.0.1:2379/v3/kv/range \
        -d "{\"key\":\"$_SKEY\",\"range_end\":\"$_SEND\"}" 2>/dev/null)

    _STATUS=$(printf '%s\n===SLOT===\n%s\n' "$_REG" "$_SLOT" | python3 "$ROOT/tests/smoke_etcd_parse.py" 2>/dev/null)
    _OK=$(echo "$_STATUS" | head -1 | grep -o 'OK=[01]' | cut -d= -f2)

    if [ "$_OK" = "1" ]; then
        _SUMMARY=$(echo "$_STATUS" | head -1 | sed 's/OK=1 //')
        echo -e "  ${GREEN}✅${NC} 注册中心健康  $_SUMMARY"
        echo "$_STATUS" | tail -n +2
        PASS=$((PASS+1))
    else
        echo -e "  ${RED}❌${NC} 注册中心异常!"
        echo "$_STATUS"
        FAIL=$((FAIL+1))
    fi
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
