#!/bin/bash
# 打印 Thunder 节点最近日志
# 用法: ./tests/logs.sh [--logic|--interface|--hello|--etcd] [行数] [过滤词]
LINES=10; FILTER=""
NODES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --logic)       NODES+=(logic) ;;
        --interface)   NODES+=(interface) ;;
        --hello)       NODES+=(hello) ;;
        --hello-ws)    NODES+=(hello_ws) ;;
        --hello-https) NODES+=(hello_https) ;;
        --etcd)        NODES+=(etcd) ;;
        --all)         NODES=(logic interface hello hello_ws hello_https) ;;
        ''|*[!0-9]*)   FILTER="$1" ;;
        *)             LINES="$1" ;;
    esac
    shift
done
[ ${#NODES[@]} -eq 0 ] && NODES=(logic interface hello hello_ws hello_https)

P=/thunder/deploy

for node in "${NODES[@]}"; do
    case "$node" in
    logic)
        echo "━━━ LOGIC (Logic/Logic_robot.log) ━━━"
        docker exec thunder-deploy-logic-1 tail -"$LINES" $P/Logic/log/Logic_robot.log 2>/dev/null \
            | { [ -n "$FILTER" ] && grep "$FILTER" || cat; } | sed 's/^/  /'
        ;;
    interface)
        echo "━━━ INTERFACE (Interface/Interface_robot.log) ━━━"
        docker exec thunder-deploy-interface-1 tail -"$LINES" $P/Interface/log/Interface_robot.log 2>/dev/null \
            | { [ -n "$FILTER" ] && grep "$FILTER" || cat; } | sed 's/^/  /'
        ;;
    hello)
        echo "━━━ HELLO (HelloHttp/Hello_robot.log) ━━━"
        docker exec thunder-deploy-hello-1 tail -"$LINES" $P/HelloHttp/log/Hello_robot.log 2>/dev/null \
            | { [ -n "$FILTER" ] && grep "$FILTER" || cat; } | sed 's/^/  /'
        ;;
    hello_ws)
        echo "━━━ HELLO_WS (HelloWs/Hello_ws_robot.log) ━━━"
        docker exec thunder-deploy-hello_ws-1 tail -"$LINES" $P/HelloWs/log/Hello_ws_robot.log 2>/dev/null \
            | { [ -n "$FILTER" ] && grep "$FILTER" || cat; } | sed 's/^/  /'
        ;;
    hello_https)
        echo "━━━ HELLO_HTTPS (HelloHttps/Hello_https_robot.log) ━━━"
        docker exec thunder-deploy-hello_https-1 tail -"$LINES" $P/HelloHttps/log/Hello_https_robot.log 2>/dev/null \
            | { [ -n "$FILTER" ] && grep "$FILTER" || cat; } | sed 's/^/  /'
        ;;
    etcd)
        echo "━━━ ETCD ━━━"
        docker exec thunder-deploy-etcd-1 etcdctl --endpoints=http://127.0.0.1:2379 endpoint health 2>/dev/null | sed 's/^/  /'
        docker exec thunder-deploy-etcd-1 etcdctl --endpoints=http://127.0.0.1:2379 endpoint status -w table 2>/dev/null | sed 's/^/  /'
        ;;
    esac
    echo ""
done
