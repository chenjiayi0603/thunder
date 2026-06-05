#!/bin/bash
# 打印所有 Thunder 节点最近日志
LINES="${1:-10}"
FILTER="${2:-}"
P="/thunder/deploy"

echo "━━━ LOGIC       (Logic/Logic_robot.log) ━━━"
docker exec thunder-deploy-logic-1 sh -c "if [ -n '$FILTER' ]; then grep '$FILTER' $P/Logic/log/Logic_robot.log | tail -$LINES; else tail -$LINES $P/Logic/log/Logic_robot.log; fi" 2>/dev/null | sed 's/^/  /'

echo "━━━ INTERFACE   (Interface/Interface_robot.log) ━━━"
docker exec thunder-deploy-interface-1 sh -c "if [ -n '$FILTER' ]; then grep '$FILTER' $P/Interface/log/Interface_robot.log | tail -$LINES; else tail -$LINES $P/Interface/log/Interface_robot.log; fi" 2>/dev/null | sed 's/^/  /'

echo "━━━ HELLO       (HelloHttp/Hello_robot.log) ━━━"
docker exec thunder-deploy-hello-1 sh -c "if [ -n '$FILTER' ]; then grep '$FILTER' $P/HelloHttp/log/Hello_robot.log | tail -$LINES; else tail -$LINES $P/HelloHttp/log/Hello_robot.log; fi" 2>/dev/null | sed 's/^/  /'

echo "━━━ HELLO_WS    (HelloWs/Hello_ws_robot.log) ━━━"
docker exec thunder-deploy-hello_ws-1 sh -c "if [ -n '$FILTER' ]; then grep '$FILTER' $P/HelloWs/log/Hello_ws_robot.log | tail -$LINES; else tail -$LINES $P/HelloWs/log/Hello_ws_robot.log; fi" 2>/dev/null | sed 's/^/  /'

echo "━━━ HELLO_HTTPS (HelloHttps/Hello_https_robot.log) ━━━"
docker exec thunder-deploy-hello_https-1 sh -c "if [ -n '$FILTER' ]; then grep '$FILTER' $P/HelloHttps/log/Hello_https_robot.log | tail -$LINES; else tail -$LINES $P/HelloHttps/log/Hello_https_robot.log; fi" 2>/dev/null | sed 's/^/  /'
