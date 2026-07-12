#!/bin/bash
set -e
PORT="${INNER_PORT:-27444}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/HelloHttps.json
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/" ./conf/HelloHttps.json
echo "Starting HelloHttps on $MY_IP:$PORT..."
./bin/HelloHttps ./conf/HelloHttps.json
EXIT=$?
if [ $EXIT -ne 0 ]; then echo "FATAL: HelloHttps exited with code $EXIT"; exit $EXIT; fi
tail -f log/Hello_robot.log 2>/dev/null &
sleep infinity
