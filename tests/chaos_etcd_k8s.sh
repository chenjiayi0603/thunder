#!/bin/bash
# etcd 混沌测试 — K8s 版本
# 用法: ./tests/chaos_etcd_k8s.sh
# 要求: K8s 集群 + thunder namespace 已部署
set -euo pipefail
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NS="${K8S_NAMESPACE:-thunder}"
ETCD_SVC="${ETCD_SVC:-thunder-etcd}"
ETCD_URL="${ETCD_URL:-http://${ETCD_SVC}.${NS}:2379}"
HOST_IP="${HOST_IP:-192.168.3.61}"
GENKEY_URL="http://${HOST_IP}:27008/Interface/gentoken"

check() { local n="$1" cmd="$2" expect="${3:-0}"; local out
  out=$(eval "$cmd" 2>/dev/null) || true
  if echo "$out" | grep -q "$expect"; then echo -e "  ${GREEN}✅${NC} $n"; PASS=$((PASS+1))
  else echo -e "  ${RED}❌${NC} $n (got: ${out:0:80})"; FAIL=$((FAIL+1)); fi
}

# helpers
etcd_health() { curl -sf --max-time 3 "${ETCD_URL}/health" 2>/dev/null || echo "down"; }
etcd_key_count() {
  curl -s --max-time 3 "${ETCD_URL}/v3/kv/range" \
    -d '{"key":"L3RodW5kZXIvcmVnaXN0cnkv","range_end":"L3RodW5kZXIvcmVnaXN0cnkw"}' 2>/dev/null \
    | python3 -c "import sys,json;print(json.load(sys.stdin).get('count',0))" 2>/dev/null || echo "0"
}
genkey_ok() { curl -sf --max-time 5 -X POST "$GENKEY_URL" -d '{"option":"GenKey"}' 2>/dev/null | python3 -c "import sys,json;j=json.load(sys.stdin);print(j.get('code',-1))" 2>/dev/null || echo "-1"; }

get_etcd_pod() { kubectl get pods -n "$NS" -l app=thunder-etcd --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null; }
get_etcd_pods() { kubectl get pods -n "$NS" -l app=thunder-etcd --field-selector=status.phase=Running -o jsonpath='{.items[*].metadata.name}' 2>/dev/null; }

echo "=============================================="
echo "  Thunder etcd 混沌测试 (K8s)"
echo "  ns=$NS  etcd=$ETCD_URL"
echo "=============================================="

# ── 0. 基线 ────────────────────────────────────
echo ""
echo "--- 0. 基线 (etcd 在线) ---"
check "etcd health"     'etcd_health' '"health":"true"'
check "GenKey 正常"     'genkey_ok' '0'
check "registry >= 4"   'etcd_key_count' '[4-9]'

# ── 1. etcd Pod 删除(单节点) → 自动重建 ─────────
echo ""
echo "--- 1. etcd 单 Pod 删除 (模拟节点故障, StatefulSet 自动重建) ---"
ETCD_POD=$(get_etcd_pod)
echo "  删除 $ETCD_POD ..."
kubectl delete pod -n "$NS" "$ETCD_POD" --grace-period=0 >/dev/null 2>&1
sleep 3
check "etcd 暂时不可达" 'etcd_health' 'down'
echo "  等待新 Pod 启动 (最多 60s)..."
kubectl wait --for=condition=Ready pod -n "$NS" -l app=thunder-etcd --timeout=60s >/dev/null 2>&1 || true
sleep 5
check "etcd 恢复健康"   'etcd_health' '"health":"true"'
sleep 10
check "GenKey 恢复"     'genkey_ok' '0'

# ── 2. 全部 etcd 停止 → 恢复 ────────────────────
echo ""
echo "--- 2. 全部 etcd 缩容到 0 → 恢复 (模拟全集群故障) ---"
echo "  缩容 etcd StatefulSet 到 0..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=0 >/dev/null 2>&1
sleep 5
check "etcd 全不可达"   'etcd_health' 'down'
echo "  恢复 etcd 到 3 副本..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=3 >/dev/null 2>&1
kubectl wait --for=condition=Ready pod -n "$NS" -l app=thunder-etcd --timeout=120s >/dev/null 2>&1 || true
sleep 20
check "etcd 恢复健康"   'etcd_health' '"health":"true"'
check "GenKey 恢复"     'genkey_ok' '0'

# ── 3. 删除 etcd PVC (模拟数据丢失) ─────────────
echo ""
echo "--- 3. 删除 etcd-0 PVC (模拟磁盘故障/数据丢失) ---"
echo "  缩容到 0 + 删除 etcd-0 PVC..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=0 >/dev/null 2>&1
sleep 5
kubectl delete pvc -n "$NS" data-thunder-etcd-0 --ignore-not-found=true >/dev/null 2>&1
echo "  恢复到 3 副本..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=3 >/dev/null 2>&1
kubectl wait --for=condition=Ready pod -n "$NS" -l app=thunder-etcd --timeout=120s >/dev/null 2>&1 || true
sleep 20
check "etcd 启动(空数据)" 'etcd_health' '"health":"true"'
echo "  等待节点重新注册 (最多 120s)..."
for i in $(seq 1 30); do
  c=$(etcd_key_count)
  [[ "$c" -ge 3 ]] && break
  sleep 4
done
check "节点重新注册"    'etcd_key_count' '[3-9]'
check "GenKey 恢复"     'genkey_ok' '0'

# ── 汇总 ───────────────────────────────────────
echo ""
echo "=============================================="
TOTAL=$((PASS+FAIL))
[ "$FAIL" -eq 0 ] && echo -e "  ${GREEN}全部通过 $PASS/$TOTAL${NC}" || echo -e "  ${RED}失败 $FAIL${NC} / 通过 ${GREEN}$PASS${NC}"
echo "=============================================="
exit $FAIL
