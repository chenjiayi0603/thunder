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
genkey_retry() { for i in $(seq 1 10); do local r=$(genkey_ok); [[ "$r" = "0" ]] && echo "0" && return 0; sleep 3; done; echo "-1"; }

get_etcd_pod() { kubectl get pods -n "$NS" -l app=thunder-etcd --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null; }
get_etcd_pod_count() { kubectl get pods -n "$NS" -l app=thunder-etcd --field-selector=status.phase=Running --no-headers 2>/dev/null | wc -l; }

# retry: keep checking until condition met or timeout
wait_for() { local desc="$1" cmd="$2" expect="$3" timeout="${4:-60}"; local t0=$(date +%s)
  while true; do
    local out; out=$(eval "$cmd" 2>/dev/null) || true
    if echo "$out" | grep -q "$expect"; then return 0; fi
    [[ $(($(date +%s)-t0)) -ge $timeout ]] && return 1
    sleep 2
  done
}

echo "=============================================="
echo "  Thunder etcd 混沌测试 (K8s)"
echo "  ns=$NS  etcd=$ETCD_URL"
echo "=============================================="

# ── 0. 基线 ────────────────────────────────────
echo ""
echo "--- 0. 基线 (etcd 在线) ---"
check "etcd health"     'etcd_health' '"health":"true"'
check "GenKey 正常"     'genkey_retry' '0'
check "registry >= 4"   'etcd_key_count' '[4-9]\|[1-9][0-9]'

# ── 1. etcd Pod 删除(单节点) → 集群自愈 ─────────
echo ""
echo "--- 1. etcd 单 Pod 删除 (3节点集群, 删1个仍有quorum, 服务不中断) ---"
ETCD_POD=$(get_etcd_pod)
echo "  删除 $ETCD_POD ..."
kubectl delete pod -n "$NS" "$ETCD_POD" --grace-period=0 >/dev/null 2>&1
# 3节点集群删1个仍健康—验证服务不中断
check "etcd 仍健康(quorum)"  'etcd_health' '"health":"true"'
check "GenKey 不中断"         'genkey_ok' '0'
echo "  等待新 Pod 重建 (最多 60s)..."
kubectl wait --for=condition=Ready pod -n "$NS" -l app=thunder-etcd --timeout=60s >/dev/null 2>&1 || true
sleep 5
check "Pod 恢复 3/3"          'get_etcd_pod_count' '3'
check "GenKey 正常"           'genkey_ok' '0'

# ── 2. 全部 etcd 缩容到 0 → 恢复 ────────────────────
echo ""
echo "--- 2. 全部 etcd 缩容到 0 → 恢复 (模拟全集群故障) ---"
echo "  缩容 etcd StatefulSet 到 0..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=0 >/dev/null 2>&1
echo "  等待 etcd 完全不可达 (最多 30s)..."
wait_for "etcd down" 'etcd_health' 'down' 30 || true
check "etcd 不可达"           'etcd_health' 'down'
echo "  恢复 etcd 到 3 副本..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=3 >/dev/null 2>&1
kubectl wait --for=condition=Ready pod -n "$NS" -l app=thunder-etcd --timeout=120s >/dev/null 2>&1 || true
sleep 20
check "etcd 恢复健康"         'etcd_health' '"health":"true"'
check "GenKey 恢复"           'genkey_ok' '0'

# ── 3. 删除 etcd-0 PVC (模拟数据丢失) ─────────────
echo ""
echo "--- 3. 删除 etcd-0 PVC (模拟磁盘故障/数据丢失) ---"
echo "  缩容到 0 + 删除 etcd-0 PVC + PV..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=0 >/dev/null 2>&1
wait_for "etcd down" 'etcd_health' 'down' 30 || true
kubectl delete pvc -n "$NS" data-thunder-etcd-0 --ignore-not-found=true >/dev/null 2>&1
kubectl delete pv pv-thunder-etcd-0 --ignore-not-found=true >/dev/null 2>&1
# 重建 PV (PVC 删除后 StatefulSet 需要同名 PV)
cat > /tmp/pv-etcd0.yaml << 'YAML'
apiVersion: v1
kind: PersistentVolume
metadata:
  name: pv-thunder-etcd-0
spec:
  capacity: {storage: 1Gi}
  accessModes: [ReadWriteOnce]
  hostPath: {path: /data/thunder/etcd-0}
  persistentVolumeReclaimPolicy: Retain
YAML
kubectl apply -f /tmp/pv-etcd0.yaml >/dev/null 2>&1
echo "  恢复到 3 副本..."
kubectl scale statefulset -n "$NS" thunder-etcd --replicas=3 >/dev/null 2>&1
echo "  等待 etcd-0 PVC 重建 + Pod Ready (最多 120s)..."
kubectl wait --for=condition=Ready pod -n "$NS" -l app=thunder-etcd --timeout=120s >/dev/null 2>&1 || true
sleep 15
check "etcd 启动"        'etcd_health' '"health":"true"'
echo "  等待节点重新注册 (最多 180s)..."
wait_for "nodes re-registered" 'etcd_key_count' '[3-9]' 180 || true
check "节点重新注册"    'etcd_key_count' '[3-9]'
check "GenKey 恢复"     'genkey_retry' '0'

# ── 汇总 ───────────────────────────────────────
echo ""
echo "=============================================="
TOTAL=$((PASS+FAIL))
[ "$FAIL" -eq 0 ] && echo -e "  ${GREEN}全部通过 $PASS/$TOTAL${NC}" || echo -e "  ${RED}失败 $FAIL${NC} / 通过 ${GREEN}$PASS${NC}"
echo "=============================================="
exit $FAIL
