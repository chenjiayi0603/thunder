#!/bin/bash
set -e

PORT="${INNER_PORT:-27009}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/Interface.json
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
# 修复: 原 sed 只匹配 "0.0.0.0", 但 conf 里可能写 "127.0.1.1" 等任意值 → 不匹配 → 绑定失败
# 现在匹配任意 inner_host 值
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"[^\"]*\"/\"inner_host\": \"$MY_IP\"/" ./conf/Interface.json
[ -n "$ETCD_ENDPOINT" ] && sed -i "s|\"etcd_endpoints\": \"[^\"]*\"|\"etcd_endpoints\": \"$ETCD_ENDPOINT\"|" ./conf/Interface.json
echo "Starting Interface on $MY_IP:$PORT..."
# Recreate 策略下旧进程释放端口可能有 TIME_WAIT, 加重试
RETRIES=0
for i in $(seq 1 5); do
    ./bin/Interface ./conf/Interface.json && break
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 98 ]; then
        echo "Port $PORT in use, retry $i/5..."
        RETRIES=$i
        sleep 2
    else
        echo "FATAL: Interface exited with code $EXIT_CODE"
        exit $EXIT_CODE
    fi
done
# 修复: 5 次 EADDRINUSE 后不应继续 (原代码会 tail+sleep 导致 Pod 假 Running)
if [ "$RETRIES" -ge 5 ] 2>/dev/null; then
    echo "FATAL: Interface failed to bind after 5 retries"
    exit 98
fi
tail -f log/Interface_robot.log 2>/dev/null &
sleep infinity
