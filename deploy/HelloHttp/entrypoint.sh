#!/bin/bash
set -e

PORT="${INNER_PORT:-27007}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/Hello.json
# hostNetwork Pod 需要真实 pod IP 才能被连接到 (0.0.0.0 会被解析到 127.0.0.1)
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/" ./conf/Hello.json
[ -n "$ETCD_ENDPOINT" ] && sed -i "s|\"etcd_endpoints\": \"[^\"]*\"|\"etcd_endpoints\": \"$ETCD_ENDPOINT\"|" ./conf/Hello.json
echo "Starting HelloHttp on $MY_IP:$PORT..."
./bin/HelloHttp ./conf/Hello.json
EXIT=$?
if [ $EXIT -ne 0 ]; then
    echo "FATAL: HelloHttp exited with code $EXIT"
    exit $EXIT
fi
echo "HelloHttp daemonized; streaming logs + keep-alive..."
tail -f log/Hello_robot.log 2>/dev/null &
# daemon 化后 PID 1 需感知 Worker 退出 (进程名 <server_name>_Loader / <server_name>_W<n>, 不匹配本 tail)
# daemon fork + setproctitle 有延迟, 先等进程出现 (最长 30s) 再进入监控
for _i in $(seq 1 30); do pgrep -f 'Hello_robot_' >/dev/null 2>&1 && break; sleep 1; done
if ! pgrep -f 'Hello_robot_' >/dev/null 2>&1; then
    echo "FATAL: Hello_robot never started"
    exit 1
fi
while pgrep -f 'Hello_robot_' >/dev/null 2>&1; do sleep 5; done
echo "FATAL: Hello_robot exited, container will restart"
exit 1
