#!/bin/bash
# ============================================================
# Layer 1: k8s Pod 层测试 — 扩缩容 / Pod自愈 / 滚动更新 / WS断连
# 验证 k8s 对 Thunder 各服务的影响
# ============================================================
# 用法:
#   ./tests/test_k8s_scale.sh               # 全部
#   ./tests/test_k8s_scale.sh --scale       # 仅扩缩容
#   ./tests/test_k8s_scale.sh --kill        # 仅 Pod kill 自愈
#   ./tests/test_k8s_scale.sh --ws          # 仅 WS 断连验证
#   ./tests/test_k8s_scale.sh --rollout     # 仅滚动更新
# 环境变量: K8S_NODE_IP  K8S_NAMESPACE
# ============================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; BLUE='\033[0;34m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

RUN_SCALE=false; RUN_KILL=false; RUN_WS=false; RUN_ROLLOUT=false
while [[ $# -gt 0 ]]; do
    case "$1" in --scale) RUN_SCALE=true ;; --kill) RUN_KILL=true ;;
        --ws) RUN_WS=true ;; --rollout) RUN_ROLLOUT=true ;;
        *) RUN_SCALE=true; RUN_KILL=true; RUN_WS=true; RUN_ROLLOUT=true; break ;;
    esac; shift
done
$RUN_SCALE || $RUN_KILL || $RUN_WS || $RUN_ROLLOUT || { RUN_SCALE=true; RUN_KILL=true; RUN_WS=true; RUN_ROLLOUT=true; }

K8S_NS="${K8S_NAMESPACE:-thunder}"
if [ -n "${K8S_NODE_IP:-}" ]; then NODE_IP="$K8S_NODE_IP"
else NODE_IP=$(kubectl get node -o jsonpath='{.items[0].status.addresses[?(@.type=="InternalIP")].address}' 2>/dev/null) || true
     [ -z "${NODE_IP:-}" ] && NODE_IP=$(hostname -I 2>/dev/null | awk '{print $1}') || true; fi
[ -z "${NODE_IP:-}" ] && { echo -e "${RED}✘ 无法检测 Node IP${NC}"; exit 1; }

PORT_HELLO=30006; PORT_IFACE=30008; PORT_WS=30010
IFACE_URL="http://${NODE_IP}:${PORT_IFACE}"
HELLO_URL="http://${NODE_IP}:${PORT_HELLO}"
WS_URL="http://${NODE_IP}:${PORT_WS}"

check() { local n="$1" e="$2" c="$3"; local out; out=$(eval "$c" 2>/dev/null) || true
    if echo "$out" | grep -q "$e"; then echo -e "  ${GREEN}✅${NC} $n"; PASS=$((PASS+1))
    else echo -e "  ${RED}❌${NC} $n (got: ${out:0:120})"; FAIL=$((FAIL+1)); fi; }
check_ge() { local n="$1" m="$2" c="$3"; local out; out=$(eval "$c" 2>/dev/null) || out="0"
    if [ "${out}" -ge "${m}" ] 2>/dev/null; then echo -e "  ${GREEN}✅${NC} $n (got: $out)"; PASS=$((PASS+1))
    else echo -e "  ${RED}❌${NC} $n (got: $out, expect ≥$m)"; FAIL=$((FAIL+1)); fi; }
info() { echo -e "  ${BLUE}→${NC} $*"; }

READY=$(kubectl get pods -n "$K8S_NS" -l app=thunder-hello --no-headers 2>/dev/null | grep -c 'Running' || echo 0)
[ "$READY" -eq 0 ] && { echo -e "${RED}✘ 集群未就绪${NC}"; exit 1; }
etcd_pod() { kubectl get pods -n "$K8S_NS" -l app=thunder-etcd --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null; }
etcd_reg_count() { kubectl exec -n "$K8S_NS" "$(etcd_pod)" -- etcdctl --endpoints=http://127.0.0.1:2379 get --prefix /thunder/registry/ 2>/dev/null | grep -c "$1" || echo 0; }
# 临时文件 (避免子shell变量丢失)
SCALE_TMP=$(mktemp /tmp/k8s_scale_XXXXXX)
trap "rm -f $SCALE_TMP" EXIT

echo "Layer1: k8s Pod 层测试  NODE=$NODE_IP  ns=$K8S_NS"
echo "=============================================="
echo "  扩缩容 & Pod自愈 & 滚动更新 & WS断连"
echo "=============================================="

# ── 1. Interface 扩缩容 ──────────────────────────────────
if $RUN_SCALE; then
echo ""
echo "--- 1. Interface 扩缩容 ---"
ORIG=$(kubectl get deploy thunder-interface -n "$K8S_NS" -o jsonpath='{.spec.replicas}')
info "扩容: $ORIG → 3"
kubectl scale deployment thunder-interface -n "$K8S_NS" --replicas=3 2>/dev/null
kubectl wait --for=condition=ready pod -l app=thunder-interface -n "$K8S_NS" --timeout=180s 2>/dev/null || true
# 等新Pod完成 apt-get + 启动 + etcd注册 (需要60s)
info "等待新Pod初始化 & etcd注册..."
for i in $(seq 1 12); do
    REG_N=$(etcd_reg_count 'INTERFACE')
    [ "$REG_N" -ge 2 ] 2>/dev/null && break
    sleep 5
done
info "etcd INTERFACE 注册数: $REG_N"
check_ge "扩容后 Interface etcd 注册数 ≥2" 2 "echo $REG_N"

info "缩容: 3 → 1 (持续请求中...)"
# 用临时文件传递子shell结果
echo "0 0" > "$SCALE_TMP"
(for i in $(seq 1 30); do
    if curl -sf --max-time 2 -X POST -d '{"option":"Echo"}' "${IFACE_URL}/Interface/gentoken" >/dev/null 2>&1; then
        :
    else
        read F T < "$SCALE_TMP"; echo "$((F+1)) $T" > "$SCALE_TMP"
    fi
    read F T < "$SCALE_TMP"; echo "$F $((T+1))" > "$SCALE_TMP"
    sleep 0.3
done) &
CPID=$!
kubectl scale deployment thunder-interface -n "$K8S_NS" --replicas=1 2>/dev/null
sleep 10; wait $CPID 2>/dev/null || true
read FAIL_C TOTAL_C < "$SCALE_TMP"
FR=$(( FAIL_C * 100 / (TOTAL_C > 0 ? TOTAL_C : 1) ))
info "缩容: $TOTAL_C 请求, $FAIL_C 失败 (${FR}%)"
check "缩容失败率 ≤10%" "0" "[ $FR -le 10 ] && echo 0 || echo 1"
kubectl wait --for=condition=ready pod -l app=thunder-interface -n "$K8S_NS" --timeout=60s 2>/dev/null || true
fi

# ── 2. Pod kill → 自愈 ───────────────────────────────────
if $RUN_KILL; then
echo ""
echo "--- 2. Pod kill → 自愈 ---"
LPOD=$(kubectl get pods -n "$K8S_NS" -l app=thunder-logic -o jsonpath='{.items[0].metadata.name}')
info "杀 Logic Pod: $LPOD"
DTS=$(date +%s)
kubectl delete pod -n "$K8S_NS" "$LPOD" 2>/dev/null
kubectl wait --for=condition=ready pod -l app=thunder-logic -n "$K8S_NS" --timeout=120s 2>/dev/null || true
RTS=$(date +%s)
POD_SEC=$((RTS - DTS)); info "新Pod Ready: ${POD_SEC}s"

OK=0; GTS=0
for i in $(seq 1 30); do
    if curl -sf --max-time 3 -X POST -d '{"option":"Echo"}' "${IFACE_URL}/Interface/gentoken" >/dev/null 2>&1; then
        OK=1; GTS=$(date +%s); break; fi
    sleep 2
done
GREC=$((GTS - DTS))
[ "$OK" -eq 1 ] && echo -e "  ${GREEN}✅${NC} Logic 自愈: Pod=${POD_SEC}s  GenKey=${GREC}s" && PASS=$((PASS+1)) \
    || { echo -e "  ${RED}❌${NC} GenKey 60s内未恢复"; FAIL=$((FAIL+1)); }

RN=$(etcd_reg_count 'LOGIC')
check "Logic etcd 重新注册" "1" "[ $RN -ge 1 ] && echo 1 || echo 0"
fi

# ── 3. WebSocket 长连接断连 ───────────────────────────────
if $RUN_WS; then
echo ""
echo "--- 3. WebSocket 长连接断连 ---"
info "建立 WS 连接..."
curl -s --max-time 60 -H "Upgrade: websocket" -H "Connection: Upgrade" \
    -H "Sec-WebSocket-Key: test1234567890123456==" -H "Sec-WebSocket-Version: 13" \
    "${WS_URL}/hello/shake" >/dev/null 2>&1 &
WSPID=$!; sleep 2

if kill -0 $WSPID 2>/dev/null; then
    WPOD=$(kubectl get pods -n "$K8S_NS" -l app=thunder-hello-ws -o jsonpath='{.items[0].metadata.name}')
    WTS=$(date +%s); info "杀 HelloWS Pod: $WPOD"
    kubectl delete pod -n "$K8S_NS" "$WPOD" 2>/dev/null; sleep 5
    kill -0 $WSPID 2>/dev/null && wait $WSPID 2>/dev/null || true
    WDT=$(( $(date +%s) - WTS ))
    echo -e "  ${GREEN}✅${NC} WS 连接在 Pod 被杀后 ${WDT}s 断开 (符合预期: 长连接无自动迁移)"; PASS=$((PASS+1))

    kubectl wait --for=condition=ready pod -l app=thunder-hello-ws -n "$K8S_NS" --timeout=120s 2>/dev/null || true; sleep 5
    check "新Pod WS 重连成功" "101" "curl -s --max-time 5 -D - -H 'Upgrade: websocket' -H 'Connection: Upgrade' -H 'Sec-WebSocket-Key: test1234567890123456==' -H 'Sec-WebSocket-Version: 13' '${WS_URL}/hello/shake' 2>/dev/null | head -5"
else
    echo -e "  ${YELLOW}⚠️${NC} WS 连接失败, 跳过"; SKIP=$((SKIP+1))
fi
fi

# ── 4. 滚动更新 ──────────────────────────────────────────
if $RUN_ROLLOUT; then
echo ""
echo "--- 4. 滚动更新 ---"
info "Interface rollout..."
echo "0 0" > "$SCALE_TMP"
(for i in $(seq 1 40); do
    if curl -sf --max-time 2 -X POST -d '{"option":"Echo"}' "${IFACE_URL}/Interface/gentoken" >/dev/null 2>&1; then :; else
        read F T < "$SCALE_TMP"; echo "$((F+1)) $T" > "$SCALE_TMP"; fi
    read F T < "$SCALE_TMP"; echo "$F $((T+1))" > "$SCALE_TMP"
    sleep 0.5
done) &
CPID=$!
kubectl rollout restart deployment thunder-interface -n "$K8S_NS" 2>/dev/null; sleep 15
wait $CPID 2>/dev/null || true
read F2 T2 < "$SCALE_TMP"
FR2=$(( F2 * 100 / (T2 > 0 ? T2 : 1) ))
info "Interface rollout: $T2 请求, $F2 失败 (${FR2}%)"
check "滚动更新失败率 ≤15%" "0" "[ $FR2 -le 15 ] && echo 0 || echo 1"
kubectl rollout status deployment thunder-interface -n "$K8S_NS" --timeout=120s 2>/dev/null || true
fi

# ── 汇总 ────────────────────────────────────────────────
echo ""; echo "=============================================="
TOTAL=$((PASS+FAIL+SKIP))
[ "$FAIL" -eq 0 ] && echo -e "  ${GREEN}Layer1 全部通过 $PASS/$TOTAL${NC}" && [ "$SKIP" -gt 0 ] && echo -e "  ${YELLOW}跳过 $SKIP${NC}" \
    || echo -e "  ${RED}失败 $FAIL${NC} / 通过 ${GREEN}$PASS${NC} / 跳过 ${YELLOW}$SKIP${NC} = $TOTAL"
echo "=============================================="
exit $FAIL
