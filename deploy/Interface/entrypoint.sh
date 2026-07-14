#!/bin/bash
set -e
PORT="${INNER_PORT:-27009}"
[ -n "$INNER_PORT" ] && sed -i "s/\"inner_port\": [0-9]*/\"inner_port\": $INNER_PORT/" ./conf/Interface.json
MY_IP="${POD_IP:-$(hostname -i 2>/dev/null || echo '0.0.0.0')}"
[ "$MY_IP" != "0.0.0.0" ] && sed -i "s/\"inner_host\": \"0.0.0.0\"/\"inner_host\": \"$MY_IP\"/" ./conf/Interface.json
[ -n "$ETCD_ENDPOINT" ] && sed -i "s|\"etcd_endpoints\": \"[^\"]*\"|\"etcd_endpoints\": \"$ETCD_ENDPOINT\"|" ./conf/Interface.json
echo "Starting Interface on $MY_IP:$PORT..."
./bin/Interface ./conf/Interface.json
EXIT=$?
if [ $EXIT -ne 0 ]; then echo "FATAL: Interface exited with code $EXIT"; exit $EXIT; fi
tail -f log/Interface_robot.log 2>/dev/null &
sleep infinity
