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
    check "etcd health" \
        '"health":"true"' \
        'curl -sf --max-time 3 http://127.0.0.1:2379/health'

    # ── 路由下发 + node_id 分配验证 ──
    # 一次 range 拉取 /thunder/registry/ prefix, Python 解析出:
    #   count | orphans | types | node_ids_csv | node_id_ok
    # node_id_ok=1 当且仅当: 所有 node_id 在 [1,255], 唯一无重复, slot/registry 一一对应
    _REG=$(curl -sf --max-time 3 http://127.0.0.1:2379/v3/kv/range \
        -d "$(python3 -c "import base64;
print('{\"key\":\"%s\",\"range_end\":\"%s\"}' % (
    base64.b64encode(b'/thunder/registry/').decode(),
    base64.b64encode(b'/thunder/registry0').decode()))")" 2>/dev/null)

    # 同时拉 slot prefix 做交叉校验
    _SLOT=$(curl -sf --max-time 3 http://127.0.0.1:2379/v3/kv/range \
        -d "$(python3 -c "import base64;
print('{\"key\":\"%s\",\"range_end\":\"%s\"}' % (
    base64.b64encode(b'/thunder/slot/').decode(),
    base64.b64encode(b'/thunder/slot0').decode()))")" 2>/dev/null)

    # stdin: registry JSON + '\\n===SLOT===\\n' + slot JSON, Python 按分隔符切分
    _REG_VALUES=$(printf '%s\n===SLOT===\n%s\n' "$_REG" "$_SLOT" | python3 -c "
import sys,json,base64
raw=sys.stdin.read()
parts=raw.split('\n===SLOT===\n',1)
try:
    reg_d=json.loads(parts[0].strip()) if parts[0].strip() else {}
    slot_d=json.loads(parts[1].strip()) if len(parts)>1 and parts[1].strip() else {}
    kvs=reg_d.get('kvs',[])
    count=int(reg_d.get('count','0'))
    orphans=sum(1 for kv in kvs if kv.get('lease','0')=='0')
    types=set(); node_ids=[]
    for kv in kvs:
        key=base64.b64decode(kv['key']).decode()
        val=json.loads(base64.b64decode(kv.get('value','')))
        nid=int(val.get('node_id',0))
        node_ids.append(nid)
        types.add(val.get('node_type',''))
    # node_id 校验
    nid_ok=1
    if len(node_ids) > len(set(node_ids)): nid_ok=0  # 重复
    for nid in node_ids:
        if nid < 1 or nid > 255: nid_ok=0; break
    # slot↔registry 交叉校验
    slot_ips=set()
    for kv in slot_d.get('kvs',[]):
        slot_ips.add(base64.b64decode(kv.get('value','')).decode())
    reg_ips=set()
    for kv in kvs:
        key=base64.b64decode(kv['key']).decode()
        reg_ips.add(key.replace('/thunder/registry/',''))
    if slot_ips and reg_ips and slot_ips != reg_ips: nid_ok=0
    print('%d %d %s %s %d' % (count, orphans, ','.join(sorted(types)),
          ','.join(str(n) for n in sorted(node_ids)), nid_ok))
except Exception as e: print('0 0 - - 0', file=sys.stderr); print('0 0')
" 2>/dev/null)

    _REG_COUNT=$(echo "$_REG_VALUES" | awk '{print $1}')
    _REG_ORPHAN=$(echo "$_REG_VALUES" | awk '{print $2}')
    _REG_TYPES=$(echo  "$_REG_VALUES" | awk '{print $3}')
    _REG_NIDS=$(echo   "$_REG_VALUES" | awk '{print $4}')
    _REG_NIDOK=$(echo  "$_REG_VALUES" | awk '{print $5}')

    check "registry 注册键数 (≥3, LOGIC+HELLO+INTERFACE)" \
        "3" \
        "echo $_REG_COUNT | awk '\$1>=3{print 3}'"

    check "注册键 lease 全非 0(无孤儿键, #19 回归)" \
        "0" \
        "echo $_REG_ORPHAN"

    check "路由下发: LOGIC+HELLO+INTERFACE 均已注册" \
        "YES" \
        "echo $_REG_TYPES | grep -q 'INTERFACE' && echo $_REG_TYPES | grep -q 'HELLO' && echo $_REG_TYPES | grep -q 'LOGIC' && echo YES"

    check "node_id 分配: 唯一, 范围[1,255], slot 对应" \
        "1" \
        "echo $_REG_NIDOK"

    check "node_id: $_REG_NIDS" \
        "$_REG_NIDS" \
        "echo $_REG_NIDS"
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
