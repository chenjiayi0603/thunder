#!/bin/bash
# etcd 混沌测试 — Docker Compose 版本 (断连/重启/数据丢失恢复)
# 用法: ./tests/chaos_etcd.sh  (需 Docker Compose 集群运行中)
set -euo pipefail
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0

ETCD_URL="${ETCD_URL:-${ETCD_URL}}"
GENKEY_URL="${GENKEY_URL:-${GENKEY_URL}}"

check() { local n="$1" cmd="$2" expect="${3:-0}"; local out
  out=$(eval "$cmd" 2>/dev/null) || true
  if echo "$out" | grep -q "$expect"; then echo -e "  ${GREEN}✅${NC} $n"; PASS=$((PASS+1))
  else echo -e "  ${RED}❌${NC} $n (got: ${out:0:80})"; FAIL=$((FAIL+1)); fi
}

echo "=============================================="
echo "  Thunder etcd 混沌测试"
echo "=============================================="
echo ""
echo "  ┌─────────────────────┬──────────────────────────────┬──────┐"
echo "  │        场景         │             验证             │ 目标 │"
echo "  ├─────────────────────┼──────────────────────────────┼──────┤"
echo "  │ etcd 停止→恢复      │ 路由缓存续命 + 异步重注册    │ ✅   │"
echo "  ├─────────────────────┼──────────────────────────────┼──────┤"
echo "  │ etcd 重启(保留数据) │ 数据持久 + 快速恢复          │ ✅   │"
echo "  ├─────────────────────┼──────────────────────────────┼──────┤"
echo "  │ etcd 数据清空(灾难) │ 自动重建 lease+slot+registry │ ✅   │"
echo "  └─────────────────────┴──────────────────────────────┴──────┘"
echo ""

# ── 0. 基线 ────────────────────────────────────
echo ""
echo "--- 0. 基线 (etcd 在线) ---"
check "etcd health"     'curl -sf --max-time 3 ${ETCD_URL}/health' '"health":"true"'
check "GenKey 正常"     'curl -sf --max-time 5 ${GENKEY_URL} -d "{\"option\":\"GenKey\"}"' '"code":0'
check "registry 存在"  'curl -s --max-time 3 ${ETCD_URL}/v3/kv/range -d "{\"key\":\"L3RodW5kZXIvcmVnaXN0cnkv\",\"range_end\":\"L3RodW5kZXIvcmVnaXN0cnkw\"}" | python3 -c "import sys,json;print(json.load(sys.stdin).get(\"count\",0))"' '3'

# ── 1. etcd 停 → 启 ───────────────────────────
# 目标: 验证 etcd 短期不可达时, 路由缓存(shm)能续命, etcd 恢复后节点自动重注册
echo ""
echo "--- 1. etcd 停止 → 恢复 (目标: 路由缓存续命+异步重注册) ---"
echo "  停止 etcd..."
docker stop thunder-deploy-etcd-1 >/dev/null 2>&1
sleep 3
check "etcd 不可达"    'curl -sf --max-time 2 ${ETCD_URL}/health || echo "down"' 'down'
check "GenKey 仍可路由(缓存)" 'curl -sf --max-time 5 ${GENKEY_URL} -d "{\"option\":\"GenKey\"}" | python3 -c "import sys,json;print(json.load(sys.stdin).get(\"code\",1))"' '0'

echo "  启动 etcd..."
docker start thunder-deploy-etcd-1 >/dev/null 2>&1
sleep 5
check "etcd 恢复"      'curl -sf --max-time 3 ${ETCD_URL}/health' '"health":"true"'
sleep 5
check "registry 恢复"  'curl -s --max-time 3 ${ETCD_URL}/v3/kv/range -d "{\"key\":\"L3RodW5kZXIvcmVnaXN0cnkv\",\"range_end\":\"L3RodW5kZXIvcmVnaXN0cnkw\"}" | python3 -c "import sys,json;print(json.load(sys.stdin).get(\"count\",0))"' '3'
check "GenKey 恢复"    'curl -sf --max-time 5 ${GENKEY_URL} -d "{\"option\":\"GenKey\"}"' '"code":0'

# ── 2. etcd 重启(保留数据) ─────────────────────
# 目标: 验证 etcd 重启后数据不丢失, raft log 完整, 节点正常续租
echo ""
echo "--- 2. etcd 重启(保留数据) (目标: 数据持久+快速恢复) ---"
docker restart thunder-deploy-etcd-1 >/dev/null 2>&1
sleep 5
check "etcd 健康"      'curl -sf --max-time 3 ${ETCD_URL}/health' '"health":"true"'
check "keys 仍在"      'curl -s --max-time 3 ${ETCD_URL}/v3/kv/range -d "{\"key\":\"L3RodW5kZXIvcmVnaXN0cnkv\",\"range_end\":\"L3RodW5kZXIvcmVnaXN0cnkw\"}" | python3 -c "import sys,json;print(json.load(sys.stdin).get(\"count\",0))"' '3'

# ── 3. etcd 数据清空重置 ────────────────────────
# 目标: 验证最坏情况(etcd 数据全部丢失), 节点能从零重建租约+slot+registry, 路由恢复
echo ""
echo "--- 3. etcd 数据清空(模拟灾难恢复) (目标: 从零重建注册表) ---"
docker compose -p thunder-deploy -f "$(dirname "$0")/../docker/docker-compose.yml" stop etcd >/dev/null 2>&1
docker run --rm -v "$(dirname "$0")/../docker/data/etcd:/d" alpine sh -c 'rm -rf /d/member' 2>/dev/null
docker compose -p thunder-deploy -f "$(dirname "$0")/../docker/docker-compose.yml" start etcd >/dev/null 2>&1
sleep 5
check "etcd 启动(空)"  'curl -sf --max-time 3 ${ETCD_URL}/health' '"health":"true"'
sleep 30
check "节点重新注册"   'curl -s --max-time 3 ${ETCD_URL}/v3/kv/range -d "{\"key\":\"L3RodW5kZXIvcmVnaXN0cnkv\",\"range_end\":\"L3RodW5kZXIvcmVnaXN0cnkw\"}" | python3 -c "import sys,json;print(json.load(sys.stdin).get(\"count\",0))"' '3'
    for i in $(seq 1 10); do c=$(curl -s --max-time 3 ${ETCD_URL}/v3/kv/range -d "{\"key\":\"L3RodW5kZXIvcmVnaXN0cnkv\",\"range_end\":\"L3RodW5kZXIvcmVnaXN0cnkw\"}" | python3 -c "import sys,json;print(json.load(sys.stdin).get(\"count\",0))" 2>/dev/null); [ "$c" = "3" ] && break; sleep 3; done
check "GenKey 恢复"    'curl -sf --max-time 5 ${GENKEY_URL} -d "{\"option\":\"GenKey\"}"' '"code":0'

# ── 汇总 ───────────────────────────────────────
echo ""
echo "=============================================="
TOTAL=$((PASS+FAIL))
[ "$FAIL" -eq 0 ] && echo -e "  ${GREEN}全部通过 $PASS/$TOTAL${NC}" || echo -e "  ${RED}失败 $FAIL${NC} / 通过 ${GREEN}$PASS${NC}"
echo "=============================================="
exit $FAIL
