#!/bin/bash
# 冒烟测试 — 核心链路快速验证(需集群已运行)
# 用法: ./tests/test_smoke.sh [--build]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'; PASS=0; FAIL=0

if [ "${1:-}" = "--build" ]; then
  echo "=== 构建 ==="; cmake --build build -j1; shift
fi

check() { # $1=name $2=expected_pattern $3=curl_cmd
  if eval "$3" 2>/dev/null | grep -q "$2"; then
    echo -e "  ${GREEN}✅${NC} $1"; PASS=$((PASS+1))
  else
    echo -e "  ${RED}❌${NC} $1"; FAIL=$((FAIL+1))
  fi
}

echo "=============================================="
echo "  冒烟测试 (需 Docker 集群)"
echo "=============================================="

# 检测集群
ss -tln 2>/dev/null | grep -q ':27006 ' || { echo "集群未启动, 退出"; exit 1; }

# TCP 模式冒烟
check "HTTP Echo"       '"code":0'  'curl -sf http://127.0.0.1:27006/hello/hello -d "{\"option\":\"Echo\"}"'
check "HTTP PoolCpu"    '"checksum":786432' 'curl -sf http://127.0.0.1:27006/hello/hello -d "{\"option\":\"TestHelloPoolCpu\"}"'
check "GenKey"          '"code":0'  'curl -sf http://127.0.0.1:27008/Interface/gentoken -d "{\"option\":\"GenKey\"}"'
check "Center Raft"     '"leader"'  'curl -sf http://127.0.0.1:26000/admin -d "{\"cmd\":\"show\",\"args\":[\"center\"]}"'
check "VerifyKey(bad)"  '"code":1'  'curl -sf http://127.0.0.1:27008/Interface/gentoken -d "{\"option\":\"VerifyKey\",\"token\":\"bad\",\"key\":\"bad\"}"'
check "WebSocket握手"   'HTTP/1.1 101' 'curl -sI -H "Upgrade: websocket" -H "Connection: Upgrade" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" http://127.0.0.1:27010/hello/shake 2>&1'

# etcd 冒烟
if curl -sf http://127.0.0.1:2379/health >/dev/null 2>&1; then
  echo ""
  echo "--- etcd 冒烟 ---"
  check "etcd health"   '"health":"true"' 'curl -sf http://127.0.0.1:2379/health'
  check "etcd 注册节点"  '"count":"2"' 'curl -sf -X POST http://127.0.0.1:2379/v3/kv/range -H "Content-Type: application/json" -d "{\"key\":\"L3RodW5kZXIvcmVnaXN0cnkvIiw\"range_end\":\"L3RodW5kZXIvcmVnaXN0cnkw\"}"'
fi

echo ""
echo "=============================================="
[ "$FAIL" -eq 0 ] && echo -e "  ${GREEN}全部通过 $PASS/$PASS${NC}" || echo -e "  ${RED}$FAIL 失败${NC} / ${GREEN}$PASS 通过${NC}"
echo "=============================================="
exit $FAIL
