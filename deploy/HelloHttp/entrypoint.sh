#!/bin/bash
set -e
PORT="${INNER_PORT:-27007}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/Hello.json
# hostNetwork Pod 需要真实 pod IP 才能被连接到 (0.0.0.0 会被解析到 127.0.0.1)
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/" ./conf/Hello.json
echo "Starting HelloHttp on $MY_IP:$PORT..."
./bin/HelloHttp ./conf/Hello.json
EXIT=$?
if [ $EXIT -ne 0 ]; then
    echo "FATAL: HelloHttp exited with code $EXIT"
    exit $EXIT
fi
echo "HelloHttp daemonized; streaming logs + keep-alive..."
tail -f log/Hello_robot.log 2>/dev/null &
sleep infinity
