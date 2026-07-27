#!/bin/bash
set -e

PORT="${INNER_PORT:-27444}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/HelloHttps.json
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/" ./conf/HelloHttps.json
[ -n "$ETCD_ENDPOINT" ] && sed -i "s|\"etcd_endpoints\": \"[^\"]*\"|\"etcd_endpoints\": \"$ETCD_ENDPOINT\"|" ./conf/HelloHttps.json
echo "Starting HelloHttps on $MY_IP:$PORT..."
./bin/HelloHttps ./conf/HelloHttps.json
EXIT=$?
if [ $EXIT -ne 0 ]; then echo "FATAL: HelloHttps exited with code $EXIT"; exit $EXIT; fi
tail -f log/Hello_robot.log 2>/dev/null &
# daemon 化后 PID 1 需感知 Worker 退出 (进程名 <server_name>_Loader / <server_name>_W<n>)
# daemon fork + setproctitle 有延迟, 先等进程出现 (最长 30s) 再进入监控
for _i in $(seq 1 30); do pgrep -f 'Hello_https_robot_' >/dev/null 2>&1 && break; sleep 1; done
if ! pgrep -f 'Hello_https_robot_' >/dev/null 2>&1; then
    echo "FATAL: Hello_https_robot never started"
    exit 1
fi
while pgrep -f 'Hello_https_robot_' >/dev/null 2>&1; do sleep 5; done
echo "FATAL: Hello_https_robot exited, container will restart"
exit 1
