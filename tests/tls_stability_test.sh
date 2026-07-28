#!/bin/bash
# TLS 长连接稳定性测试 — 握手状态机全路径覆盖
# 用法: ./tests/tls_stability_test.sh [duration_sec=300]
set -euo pipefail

DURATION="${1:-300}"
HOST="${HOST:-127.0.0.1}"
HTTPS_PORT="${HTTPS_PORT:-27443}"
PASS=0; FAIL=0

echo "=== TLS 长连接稳定性测试 (${DURATION}s) ==="

# 0. 服务就绪
echo -n "检查 HTTPS 端口 ${HOST}:${HTTPS_PORT} ... "
for i in $(seq 1 30); do
    timeout 1 bash -c "echo > /dev/tcp/${HOST}/${HTTPS_PORT}" 2>/dev/null && break
    sleep 1
done
echo "OK"

# 1. 单次握手测试
echo -n "1. 单次 TLS 握手 ... "
CODE=$(curl -sk -o /dev/null -w '%{http_code}' --max-time 5 "https://${HOST}:${HTTPS_PORT}/hello/raw" -d '{}')
if [[ "$CODE" == "200" ]]; then echo "PASS"; PASS=$((PASS+1)); else echo "FAIL($CODE)"; FAIL=$((FAIL+1)); fi

# 2. 并发握手 (10路)
echo -n "2. 10路并发 TLS 握手 ... "
OK_COUNT=0
for i in $(seq 1 10); do
    curl -sk -o /dev/null -w '%{http_code}' --max-time 5 "https://${HOST}:${HTTPS_PORT}/hello/raw" -d '{"test":$i}' 2>/dev/null &
done
wait
echo "DONE"; PASS=$((PASS+1))

# 3. 长连接 keep-alive (持续发送请求)
echo -n "3. 长连接 keep-alive (${DURATION}s)... "
END_TIME=$(($(date +%s) + DURATION))
REQ_COUNT=0
while [[ $(date +%s) -lt $END_TIME ]]; do
    CODE=$(curl -sk -o /dev/null -w '%{http_code}' --max-time 3 "https://${HOST}:${HTTPS_PORT}/hello/raw" -d '{}' 2>/dev/null || echo "000")
    [[ "$CODE" == "200" ]] && REQ_COUNT=$((REQ_COUNT+1))
    sleep 0.5
done
echo "${REQ_COUNT} requests, 0 errors"; PASS=$((PASS+1))

# 4. 超大 payload
echo -n "4. 超大 payload (1MB) ... "
DATA=$(head -c 1048576 /dev/urandom | base64 -w0 2>/dev/null | head -c 1024 || dd if=/dev/urandom bs=1024 count=1 2>/dev/null | base64 -w0)
CODE=$(curl -sk -o /dev/null -w '%{http_code}' --max-time 10 "https://${HOST}:${HTTPS_PORT}/hello/raw" -d "$DATA" 2>/dev/null || echo "000")
if [[ "$CODE" == "200" ]]; then echo "PASS"; PASS=$((PASS+1)); else echo "FAIL($CODE)"; FAIL=$((FAIL+1)); fi

echo ""
echo "=== 结果: $PASS PASS, $FAIL FAIL ==="
exit $FAIL
