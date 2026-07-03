#!/bin/bash
# 测试前环境预检 — 所有指标必须全绿才能开始测试
# 违反规则：🚫 不允许靠重启蒙混过关

set -euo pipefail
FAIL=0

echo "═══ 测试环境预检 ═══"

# ── 1. 端口检查 ──
echo ""
echo "--- 端口 ---"
for port in 27006 27008 27443 27010 2379 2381 2383; do
    case $port in
        27006) svc="HelloHttp";;
        27008) svc="Interface";;
        27443) svc="HelloHttps";;
        27010) svc="HelloWs";;
        2379) svc="etcd1";;
        2381) svc="etcd2";;
        2383) svc="etcd3";;
    esac
    if timeout 1 bash -c "echo > /dev/tcp/127.0.0.1/$port" 2>/dev/null; then
        echo "  ✅ $svc :$port"
    else
        echo "  ❌ $svc :$port — NOT LISTENING"
        FAIL=$((FAIL+1))
    fi
done

# ── 2. etcd 注册检查 ──
echo ""
echo "--- etcd 注册 ---"
types=$(python3 -c "
import requests, base64, json
r = requests.post('http://127.0.0.1:2379/v3/kv/range', json={
    'key': base64.b64encode(b'/thunder/registry/').decode(),
    'range_end': base64.b64encode(b'/thunder/registry0').decode()
}, timeout=5)
types = set()
for kv in r.json().get('kvs',[]):
    v = json.loads(base64.b64decode(kv['value']))
    types.add(v.get('node_type'))
print(' '.join(sorted(types)))
" 2>&1)
echo "    节点类型: $types"

expected="HELLO_HTTP HELLO_HTTPS HELLO_WS INTERFACE LOGIC"
if [ "$types" = "$expected" ]; then
    echo "  ✅ 5/5 全部注册"
else
    for t in $expected; do
        if echo "$types" | grep -qw "$t"; then
            echo "  ✅ $t"
        else
            echo "  ❌ $t 未注册"
            FAIL=$((FAIL+1))
        fi
    done
fi

# ── 3. etcd 集群健康 ──
echo ""
echo "--- etcd 集群 ---"
for ep in 2379 2381 2383; do
    health=$(curl -s --max-time 3 "http://127.0.0.1:$ep/health" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('health','?'))" 2>/dev/null)
    if [ "$health" = "true" ]; then
        echo "  ✅ etcd:$ep healthy"
    else
        echo "  ❌ etcd:$ep unhealthy (got: $health)"
        FAIL=$((FAIL+1))
    fi
done

# ── 4. Worker 健康 — CPU 不能 99% ──
echo ""
echo "--- Worker CPU ---"
worker_cpu=$(docker stats --no-stream --format '{{.CPUPerc}}' thunder-deploy-hello-1 2>/dev/null | head -1 | sed 's/%//')
wo_cpu=$(echo "$worker_cpu" | awk '{print int($1)}')
if [ "$wo_cpu" -gt 90 ]; then
    echo "  ❌ Worker CPU: ${worker_cpu}% — busy loop suspected"
    FAIL=$((FAIL+1))
else
    echo "  ✅ Worker CPU: ${worker_cpu}%"
fi

# ── 结果 ──
echo ""
echo "════════════════════════"
if [ "$FAIL" -eq 0 ]; then
    echo "  ✅ 全绿，可以开始测试"
    exit 0
else
    echo "  ❌ $FAIL 项失败，必须先修复根因"
    echo "  🚫 禁止盲目重启后重跑"
    exit 1
fi
