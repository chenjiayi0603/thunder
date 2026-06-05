#!/bin/bash
# 打印 Thunder 节点最近日志
# 用法: ./tests/logs.sh [--logic|--etcd] [行数] [过滤词]
LINES=10; FILTER=""
LG=false; IF=false; HL=false; HW=false; HH=false; ET=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --logic)       LG=true ;;
        --interface)   IF=true ;;
        --hello)       HL=true ;;
        --hello-ws)    HW=true ;;
        --hello-https) HH=true ;;
        --etcd)        ET=true ;;
        --all)         LG=true; IF=true; HL=true; HW=true; HH=true ;;
        ''|*[!0-9]*)   FILTER="$1" ;;
        *)             LINES="$1" ;;
    esac
    shift
done
$LG || $IF || $HL || $HW || $HH || $ET || { LG=true; IF=true; HL=true; HW=true; HH=true; }
P=/thunder/deploy

$LG && { echo "━━━ LOGIC (Logic/Logic_robot.log) ━━━"
  [ -n "$FILTER" ] && docker exec thunder-deploy-logic-1 grep "$FILTER" $P/Logic/log/Logic_robot.log 2>/dev/null | tail -$LINES | sed 's/^/  /'
  [ -z "$FILTER" ] && docker exec thunder-deploy-logic-1 tail -$LINES $P/Logic/log/Logic_robot.log 2>/dev/null | sed 's/^/  /'
  echo ""; }

$IF && { echo "━━━ INTERFACE (Interface/Interface_robot.log) ━━━"
  [ -n "$FILTER" ] && docker exec thunder-deploy-interface-1 grep "$FILTER" $P/Interface/log/Interface_robot.log 2>/dev/null | tail -$LINES | sed 's/^/  /'
  [ -z "$FILTER" ] && docker exec thunder-deploy-interface-1 tail -$LINES $P/Interface/log/Interface_robot.log 2>/dev/null | sed 's/^/  /'
  echo ""; }

$HL && { echo "━━━ HELLO (HelloHttp/Hello_robot.log) ━━━"
  [ -n "$FILTER" ] && docker exec thunder-deploy-hello-1 grep "$FILTER" $P/HelloHttp/log/Hello_robot.log 2>/dev/null | tail -$LINES | sed 's/^/  /'
  [ -z "$FILTER" ] && docker exec thunder-deploy-hello-1 tail -$LINES $P/HelloHttp/log/Hello_robot.log 2>/dev/null | sed 's/^/  /'
  echo ""; }

$HW && { echo "━━━ HELLO_WS (HelloWs/Hello_ws_robot.log) ━━━"
  [ -n "$FILTER" ] && docker exec thunder-deploy-hello_ws-1 grep "$FILTER" $P/HelloWs/log/Hello_ws_robot.log 2>/dev/null | tail -$LINES | sed 's/^/  /'
  [ -z "$FILTER" ] && docker exec thunder-deploy-hello_ws-1 tail -$LINES $P/HelloWs/log/Hello_ws_robot.log 2>/dev/null | sed 's/^/  /'
  echo ""; }

$HH && { echo "━━━ HELLO_HTTPS (HelloHttps/Hello_https_robot.log) ━━━"
  [ -n "$FILTER" ] && docker exec thunder-deploy-hello_https-1 grep "$FILTER" $P/HelloHttps/log/Hello_https_robot.log 2>/dev/null | tail -$LINES | sed 's/^/  /'
  [ -z "$FILTER" ] && docker exec thunder-deploy-hello_https-1 tail -$LINES $P/HelloHttps/log/Hello_https_robot.log 2>/dev/null | sed 's/^/  /'
  echo ""; }

$ET && { echo "━━━ ETCD ━━━"
  echo "  === health ==="
  docker exec thunder-deploy-etcd-1 etcdctl --endpoints=http://127.0.0.1:2379 endpoint health 2>/dev/null | sed 's/^/  /'
  echo "  === registry ==="
  docker exec thunder-deploy-etcd-1 etcdctl --endpoints=http://127.0.0.1:2379 get --prefix /thunder/registry/ -w simple 2>/dev/null | sed 's/^/  /'
  echo "  === slot ==="
  docker exec thunder-deploy-etcd-1 etcdctl --endpoints=http://127.0.0.1:2379 get --prefix /thunder/slot/ -w simple 2>/dev/null | sed 's/^/  /'
  echo "  === leases ==="
  docker exec thunder-deploy-etcd-1 etcdctl --endpoints=http://127.0.0.1:2379 lease list 2>/dev/null | sed 's/^/  /'
  echo ""; }
