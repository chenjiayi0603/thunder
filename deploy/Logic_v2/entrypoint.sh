#!/bin/bash
set -e
PORT="${INNER_PORT:-16069}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/Logic.json
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/" ./conf/Logic.json
echo "Starting Logic v2 on $MY_IP:$PORT..."
./bin/Logic ./conf/Logic.json
EXIT=$?
if [ $EXIT -ne 0 ]; then echo "FATAL: Logic v2 exited with code $EXIT"; exit $EXIT; fi
tail -f log/Logic_robot.log 2>/dev/null &
sleep infinity
