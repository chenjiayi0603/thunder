#!/bin/bash
# etcd 回归测试 — 单节点 etcd 全链路验证(含 Admin)
# 用法: ./tests/e2e/test_etcd_regression.sh
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

check() {
  if [ "$2" = "$3" ]; then
    echo -e "  ${GREEN}✅${NC} $1"; PASS=$((PASS+1))
  else
    echo -e "  ${RED}❌${NC} $1 (want=$2 got=$3)"; FAIL=$((FAIL+1))
  fi
}

etcd_count() {
  local k=$(echo -n "$1" | base64 -w0) r=""
  [ "${2:-0}" = "1" ] && r=",\"range_end\":\"$(echo -n "${1:0:-1}$(printf '\\x%02x' $((16#$(printf '%x' "'${1: -1}")+1)))" | base64 -w0)\""
  curl -sf -X POST http://127.0.0.1:2379/v3/kv/range -H "Content-Type: application/json" \
    -d "{\"key\":\"$k\"$r}" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('count',0))"
}

cleanup() {
  kill $(pgrep -f "Logic_robot$") 2>/dev/null || true
  kill $(pgrep -f "Hello_") 2>/dev/null || true
  sleep 1
  docker stop thunder-etcd-regression 2>/dev/null || true
  docker rm thunder-etcd-regression 2>/dev/null || true
  cd "$ROOT" && git checkout deploy/Logic/conf/Logic.json deploy/HelloHttp/conf/Hello.json 2>/dev/null || true
}

echo "=============================================="
echo "  etcd 回归测试 (7项 + Admin)"
echo "=============================================="
trap cleanup EXIT

# [1] etcd
echo -e "\n${YELLOW}[1] etcd${NC}"
docker run -d --name thunder-etcd-regression --network host \
  quay.io/coreos/etcd:v3.5.21 etcd --name=etcd1 \
  --listen-client-urls=http://127.0.0.1:2379 --advertise-client-urls=http://127.0.0.1:2379 --enable-v2=false
sleep 3
check "health" "true" "$(curl -sf http://127.0.0.1:2379/health | python3 -c "import sys,json;print(json.load(sys.stdin)['health'])")"
curl -sf -X POST http://127.0.0.1:2379/v3/kv/deleterange \
  -H "Content-Type: application/json" -d '{"key":"L3RodW5kZXIv","range_end":"L3RodW5kZXIw"}' >/dev/null 2>&1
check "清空" "0" "$(etcd_count "/thunder/registry/" 1)"

# [2] 配置
echo -e "\n${YELLOW}[2] 配置${NC}"
python3 -c "import json;[json.dump({**json.load(open(f)),'center':{'connector':'etcd','etcd_endpoints':'http://127.0.0.1:2379'}},open(f,'w'),indent=4) for f in ['$ROOT/deploy/Logic/conf/Logic.json','$ROOT/deploy/HelloHttp/conf/Hello.json']]"

# [3] 注册
echo -e "\n${YELLOW}[3] 注册${NC}"
rm -f "$ROOT"/deploy/Logic/log/Logic_robot.log
cd "$ROOT"/deploy/Logic; ./bin/Logic conf/Logic.json &
cd "$ROOT"/deploy/HelloHttp; rm -f log/*.log; bash node.sh start 2>/dev/null
cd "$ROOT"; sleep 8
check "2节点" "2" "$(etcd_count "/thunder/registry/" 1)"

# [4] Admin
echo -e "\n${YELLOW}[4] Admin${NC}"
python3 "$ROOT"/deploy/scripts/admin_nodes.py --endpoint http://127.0.0.1:2379 2>/dev/null | tee /tmp/an.txt
check "admin_nodes 2" "2" "$(grep -c "16068\|27007" /tmp/an.txt)"
bash "$ROOT"/deploy/scripts/admin_status.sh http://127.0.0.1:2379 2>/dev/null | head -3
STATUS_OK=$(bash "$ROOT"/deploy/scripts/admin_status.sh http://127.0.0.1:2379 2>/dev/null | grep -c "health: OK" || echo 0)
check "admin_status" "1" "$STATUS_OK"

# [5] 崩溃摘除
echo -e "\n${YELLOW}[5] 崩溃摘除${NC}"
kill $(pgrep -f "Logic_robot$") 2>/dev/null; sleep 12
check "剩1个" "1" "$(etcd_count "/thunder/registry/" 1)"

# [6] 幂等恢复
echo -e "\n${YELLOW}[6] 幂等${NC}"
rm -f "$ROOT"/deploy/Logic/log/Logic_robot.log
cd "$ROOT"/deploy/Logic; ./bin/Logic conf/Logic.json & cd "$ROOT"; sleep 6
check "恢复2节点" "2" "$(etcd_count "/thunder/registry/" 1)"
NID=$(curl -sf -X POST http://127.0.0.1:2379/v3/kv/range -H "Content-Type: application/json" \
  -d '{"key":"L3RodW5kZXIvcmVnaXN0cnkvMTI3LjAuMC4xOjE2MDY4"}' 2>/dev/null \
  | python3 -c "import sys,json,base64;exec('kvs=json.load(sys.stdin).get(\"kvs\",[])\nfor kv in kvs:\n v=json.loads(base64.b64decode(kv[\"value\"]))\n if v.get(\"node_port\")==16068: print(v[\"node_id\"]);break')" 2>/dev/null)
check "幂等247" "247" "${NID:-0}"

# [7] etcd恢复
echo -e "\n${YELLOW}[7] etcd恢复${NC}"
docker stop thunder-etcd-regression; sleep 3
! curl -sf http://127.0.0.1:2379/health >/dev/null 2>&1 && check "etcd down" "down" "down" || check "etcd down" "down" "alive"
docker start thunder-etcd-regression; sleep 8
check "恢复2节点" "2" "$(etcd_count "/thunder/registry/" 1)"

# 结果
echo -e "\n=============================================="
[ "$FAIL" -eq 0 ] && echo -e "  ${GREEN}全部通过 ${PASS}/${PASS}${NC}" || echo -e "  ${RED}${FAIL}失败${NC} ${GREEN}${PASS}通过${NC}"
echo "=============================================="
exit $FAIL
