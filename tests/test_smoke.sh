#!/bin/bash
# 冒烟测试 — 核心链路快速验证（支持 docker-compose / k8s 双模式）
# 用法:
#   ./tests/test_smoke.sh              # 全部 (docker-compose 模式)
#   ./tests/test_smoke.sh --k8s        # 全部 (k8s 模式)
#   ./tests/test_smoke.sh --hello      # 仅 Hello(HTTP/HTTPS/WS/Redis/MySQL)
#   ./tests/test_smoke.sh --interface  # 仅 Interface→Logic
#   ./tests/test_smoke.sh --etcd       # 仅 etcd 注册中心
#   ./tests/test_smoke.sh --build      # 先重编再跑
#   ./tests/test_smoke.sh --k8s --hello --interface  # k8s 模式 + 指定段
#
# 环境变量:
#   K8S_NODE_IP    k8s 模式下节点 IP (默认自动检测)
#   K8S_NAMESPACE  k8s namespace (默认 thunder)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

# ── 参数解析 ─────────────────────────────────────────────
RUN_HELLO=false; RUN_INTERFACE=false; RUN_ETCD=false; K8S_MODE=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) echo "=== 构建 ==="; cmake --build build -j1 && cmake --install build ;;
        --k8s)   K8S_MODE=true ;;
        --hello) RUN_HELLO=true ;;
        --interface) RUN_INTERFACE=true ;;
        --etcd) RUN_ETCD=true ;;
        *) RUN_HELLO=true; RUN_INTERFACE=true; RUN_ETCD=true; break ;;
    esac
    shift
done
# 无参数 → 全部
if ! $RUN_HELLO && ! $RUN_INTERFACE && ! $RUN_ETCD; then
    RUN_HELLO=true; RUN_INTERFACE=true; RUN_ETCD=true
fi

# ── k8s 模式: 检测 Node IP 和 Namespace ─────────────────────
K8S_NS="${K8S_NAMESPACE:-thunder}"
if $K8S_MODE; then
    # 自动检测 Node IP
    if [ -n "${K8S_NODE_IP:-}" ]; then
        NODE_IP="$K8S_NODE_IP"
    else
        NODE_IP=$(kubectl get node -o jsonpath='{.items[0].status.addresses[?(@.type=="InternalIP")].address}' 2>/dev/null) || true
        if [ -z "${NODE_IP:-}" ]; then
            NODE_IP=$(hostname -I 2>/dev/null | awk '{print $1}') || true
        fi
    fi
    if [ -z "${NODE_IP:-}" ]; then
        echo -e "${RED}✘ 无法检测 k8s Node IP，请设置 K8S_NODE_IP 环境变量${NC}"
        exit 1
    fi
    echo "k8s 模式: NODE_IP=$NODE_IP  namespace=$K8S_NS"

    # 端口映射 (NodePort)
    PORT_HELLO=30006
    PORT_HELLO_WS=30010
    PORT_INTERFACE=30008

    # 获取 pod 名（用于 etcd exec）
    ETCD_POD=$(kubectl get pods -n "$K8S_NS" -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}' 2>/dev/null) || ETCD_POD=""
else
    # Docker-compose 模式自动检测宿主机 IP(容器 network_mode:host 绑定此 IP, 非 127.0.0.1)
    NODE_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
    if [ -z "${NODE_IP:-}" ]; then
        NODE_IP="127.0.0.1"  # 兜底
    fi
    PORT_HELLO=27006
    PORT_HELLO_WS=27010
    PORT_INTERFACE=27008
    ETCD_POD=""
fi

# ── 检查集群是否已启动 ────────────────────────────────────────
if $K8S_MODE; then
    READY=$(kubectl get pods -n "$K8S_NS" -l app=thunder-hello --no-headers 2>/dev/null | grep -c 'Running' || echo 0)
    if [ "$READY" -eq 0 ]; then
        echo -e "${RED}✘ k8s 集群未就绪 (namespace=$K8S_NS)，请先部署${NC}"
        exit 1
    fi
else
    if ! ss -tln 2>/dev/null | grep -q ':27006 '; then
        echo -e "${RED}✘ 集群未启动，请先执行 ./deploy.sh up${NC}"
        exit 1
    fi
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
if $RUN_HELLO; then
echo ""
echo "--- HTTP / Hello ---"

HELLO_URL="http://${NODE_IP}:${PORT_HELLO}"
HELLO_WS_URL="http://${NODE_IP}:${PORT_HELLO_WS}"

check "HTTP Echo (POST)                      [curl→Hello:${PORT_HELLO}]" \
    '"code":0' \
    "curl -sf --max-time 5 ${HELLO_URL}/hello/hello -d '{\"option\":\"Echo\"}'"

check "HTTP PoolCpu 协程挂起/恢复 (POST)     [curl→Hello:${PORT_HELLO}]" \
    '"checksum":786432' \
    "curl -sf --max-time 10 ${HELLO_URL}/hello/hello -d '{\"option\":\"TestHelloPoolCpu\"}'"

if $K8S_MODE; then
    check "HTTPS Echo (POST)                     [curl→HelloHttps:30043]" \
        '"code":0' \
        "curl -skf --max-time 5 https://${NODE_IP}:30043/hello/hello -d '{\"option\":\"Echo\"}'"
else
    check "HTTPS Echo (POST)                     [curl→HelloHttps:27443]" \
        '"code":0' \
        "curl -skf --max-time 5 https://${NODE_IP}:27443/hello/hello -d '{\"option\":\"Echo\"}'"
fi

# Redis: k8s 模式下需要传 k8s Service DNS，docker-compose 用 127.0.0.1 默认值
_REDIS_HOST="${K8S_REDIS_HOST:-thunder-redis.${K8S_NS}}"
_MYSQL_HOST="${K8S_MYSQL_HOST:-thunder-mysql.${K8S_NS}}"
if $K8S_MODE; then
    _REDIS_EXTRA=",\"redis_host\":\"${_REDIS_HOST}\""
    _MYSQL_EXTRA=",\"mysql_host\":\"${_MYSQL_HOST}\",\"mysql_password\":\"root123\",\"mysql_db\":\"thunder\""
else
    _REDIS_EXTRA=""
    _MYSQL_EXTRA=""
fi

check "Redis set/get (CoRedis)               [curl→Hello:${PORT_HELLO}→Redis:6379]" \
    '"set_ok":1' \
    "curl -sf --max-time 5 ${HELLO_URL}/hello/hello -d '{\"option\":\"TestHelloCoRedis\"${_REDIS_EXTRA}}'"

check "MySQL create/insert/select (CoMysql)   [curl→Hello:${PORT_HELLO}→MySQL:3306]" \
    '"select_ok":1' \
    "curl -sf --max-time 5 ${HELLO_URL}/hello/hello -d '{\"option\":\"TestHelloCoMysql\"${_MYSQL_EXTRA}}'"

# WebSocket：-D - 把响应头输出到 stdout，不用 -f（101 非 2xx，-f 会失败）
check "WebSocket 握手 101                    [curl→HelloWs:${PORT_HELLO_WS}]" \
    '101 Switching Protocols' \
    "curl -s --max-time 3 -D - \
      -H 'Upgrade: websocket' \
      -H 'Connection: Upgrade' \
      -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
      -H 'Sec-WebSocket-Version: 13' \
      ${HELLO_WS_URL}/hello/shake"

fi  # RUN_HELLO

# ── Interface → Logic 全链路 ──────────────────────────────────
if $RUN_INTERFACE; then
echo ""
echo "--- Interface → Logic ---"

IFACE_URL="http://${NODE_IP}:${PORT_INTERFACE}"

# 请求 1：GenKey 生成 token（POST）
_GENKEY_POST=$(curl -sf --max-time 5 "${IFACE_URL}/Interface/gentoken" \
  -d '{"option":"GenKey"}' 2>/dev/null)
check "GenKey (POST)                           [curl→Interface:${PORT_INTERFACE}→Logic:16068]" '"code":0' "echo '${_GENKEY_POST}'"

# 请求 2：VerifyKey 验证上一步拿到的 token
_TOKEN=$(echo "$_GENKEY_POST" | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
_KEY=$(echo   "$_GENKEY_POST" | python3 -c "import sys,json; print(json.load(sys.stdin).get('key',''))"   2>/dev/null)
check "VerifyKey 有效 token → code:0          [curl→Interface→Logic]" \
    '"code":0' \
    "curl -sf --max-time 5 ${IFACE_URL}/Interface/gentoken \
      -d '{\"option\":\"VerifyKey\",\"token\":\"${_TOKEN}\",\"key\":\"${_KEY}\"}'"

# 请求 3：GenKey（GET + JSON body）
check "GenKey (GET + JSON body)                [curl→Interface→Logic]" \
    '"code":0' \
    "curl -sf --max-time 5 -X GET \
      -H 'Content-Type: application/json' \
      ${IFACE_URL}/Interface/gentoken \
      -d '{\"option\":\"GenKey\"}'"

check "VerifyKey 非法 token → code:1          [curl→Interface→Logic]" \
    '"code":1' \
    "curl -sf --max-time 5 ${IFACE_URL}/Interface/gentoken \
      -d '{\"option\":\"VerifyKey\",\"token\":\"bad\",\"key\":\"bad\"}'"

check "VerifyKey 空 token → code:1            [curl→Interface→Logic]" \
    '"code":1' \
    "curl -s --max-time 5 ${IFACE_URL}/Interface/gentoken \
      -d '{\"option\":\"VerifyKey\",\"token\":\"\",\"key\":\"\"}'"

fi  # RUN_INTERFACE

# ── Lua Module ─────────────────────────────────────────
echo ""
echo "--- Lua Module ---"

LUA_RESP=$(curl -sf --max-time 5 "http://${NODE_IP}:${PORT_HELLO}/hello/lua_echo" -d 'test' 2>/dev/null)
check "Lua echo (POST)                         [curl->Hello:${PORT_HELLO}->Lua]" '"code":0' "echo '${LUA_RESP}'"

LUA_LIMIT_LONG=$(python3 -c "print('x'*101)")
check "Lua limit body>100 (POST)              [curl->Hello:${PORT_HELLO}->Lua]" '"body too long"' "curl -sf --max-time 5 'http://${NODE_IP}:${PORT_HELLO}/hello/lua_limit' -d '${LUA_LIMIT_LONG}'"

LUA_ROUTE_RESP=$(curl -sf --max-time 10 "http://${NODE_IP}:${PORT_HELLO}/hello/lua_route" -d '{"option":"Echo"}' 2>/dev/null)
# 必须 LOGIC 可达: {"code":0,"msg":"ok","logic":...}  (含 logic 字段=LOGIC 真实回包)
# 不可达/超时则是 {"code":1,"msg":"logic timeout"}，判失败
if echo "$LUA_ROUTE_RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('code')==0 and 'logic' in d" 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "${GREEN}  [PASS] Lua route -> LOGIC (resp=${LUA_ROUTE_RESP})${NC}"
else
    FAIL=$((FAIL+1)); echo -e "${RED}  [FAIL] Lua route -> LOGIC: ${LUA_ROUTE_RESP}${NC}"
fi

# SendToNodeType: fire-and-forget
LUA_NT_FF=$(curl -sf --max-time 5 "http://${NODE_IP}:${PORT_HELLO}/hello/lua_node_type" \
    -d '{"mode":"fire_forget"}' 2>/dev/null)
check "Lua SendToNodeType fire-and-forget (POST) [curl->Hello:${PORT_HELLO}->Lua]" \
    '"fire_forget_ok"' "echo '${LUA_NT_FF}'"

# SendToNodeType: async callback
LUA_NT_ASYNC=$(curl -sf --max-time 10 "http://${NODE_IP}:${PORT_HELLO}/hello/lua_node_type" \
    -d '{"mode":"async"}' 2>/dev/null)
if echo "$LUA_NT_ASYNC" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('code')==0 and d.get('mode')=='async'" 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "${GREEN}  [PASS] Lua SendToNodeType async (resp=${LUA_NT_ASYNC})${NC}"
else
    FAIL=$((FAIL+1)); echo -e "${RED}  [FAIL] Lua SendToNodeType async: ${LUA_NT_ASYNC}${NC}"
fi

# SendToNodeType: async with targetId
LUA_NT_TARGET=$(curl -sf --max-time 10 "http://${NODE_IP}:${PORT_HELLO}/hello/lua_node_type" \
    -d '{"mode":"async_target","targetId":"smoke_test_user"}' 2>/dev/null)
if echo "$LUA_NT_TARGET" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('code')==0 and d.get('targetId')=='smoke_test_user'" 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "${GREEN}  [PASS] Lua SendToNodeType targetId (resp=${LUA_NT_TARGET})${NC}"
else
    FAIL=$((FAIL+1)); echo -e "${RED}  [FAIL] Lua SendToNodeType targetId: ${LUA_NT_TARGET}${NC}"
fi


# ── etcd ──────────────────────────────────────────────────────
if $RUN_ETCD; then
echo ""
echo "--- etcd ---"
echo "     Manager(各节点) ──HttpCodec──► etcd(:2379)"

# k8s 模式: 通过 kubectl exec 查询 etcd
if $K8S_MODE; then
    if [ -z "${ETCD_POD:-}" ]; then
        echo -e "  ${YELLOW}⚠️ ${NC} 找不到 etcd Pod，跳过 etcd 段"
        SKIP=$((SKIP+1))
    else
        _REG=$(kubectl exec -n "$K8S_NS" "$ETCD_POD" -- \
            etcdctl --endpoints=http://127.0.0.1:2379 get --prefix /thunder/registry/ 2>/dev/null) || _REG=""
        _SLOT=$(kubectl exec -n "$K8S_NS" "$ETCD_POD" -- \
            etcdctl --endpoints=http://127.0.0.1:2379 get --prefix /thunder/slot/ 2>/dev/null) || _SLOT=""

        if [ -z "${_REG:-}" ]; then
            echo -e "  ${RED}❌${NC} etcd 查询 registry 失败"
            FAIL=$((FAIL+1))
        else
            _COUNT=$(echo "$_REG" | grep -c '"node_type"' || echo 0)
            _TYPES=$(echo "$_REG" | grep -oP '"node_type":"\K[^"]+' | sort -u | tr '\n' ',' | sed 's/,$//')
            echo -e "  ${GREEN}✅${NC} 注册中心健康  nodes=${_COUNT} types=[${_TYPES}]"
            echo "$_REG" | while IFS= read -r line; do
                if echo "$line" | grep -q '"node_type"'; then
                    _type=$(echo "$line" | python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('node_type','?'))" 2>/dev/null || echo "?")
                    _ip=$(echo "$line" | grep -oP '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1)
                    echo "    ${_type}  ${_ip}"
                fi
            done
            PASS=$((PASS+1))
        fi
    fi
else
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
fi   # if ! curl ... (docker-compose 模式)

fi   # if $K8S_MODE else

# ── etcd 监控 ──────────────────────────────────────────────────
_ADMIN="${ROOT}/deploy/scripts/admin.py"
if ! $K8S_MODE && [ -f "$_ADMIN" ] && curl -sf --max-time 2 http://127.0.0.1:2379/health >/dev/null 2>&1; then
    echo ""
    echo "--- etcd 监控 ---"
    echo ""
    echo "  === nodes ==="
    python3 "$_ADMIN" nodes 2>/dev/null | sed 's/^/  /'
    echo ""
    echo "  === routes ==="
    python3 "$_ADMIN" routes 2>/dev/null | sed 's/^/  /'
    echo ""
    echo "  === status ==="
    python3 "$_ADMIN" status 2>/dev/null | sed 's/^/  /'
fi
fi  # RUN_ETCD

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
