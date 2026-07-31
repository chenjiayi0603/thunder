#!/usr/bin/env bash
# Thunder I/O Backend Fast-Path 多轮压测
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
THUNDER_BIN="/home/tommychen/thunder/build/bin/Hello"
REAL_IP="192.168.3.61"
THUNDER_PORT=28006
NGINX_PORT=18088
DURATION=10
THREADS=4
CONN=100
THUNDER_CPUS="4-9"
WRK_CPUS="12-19"
ROUNDS=3

run_one() {
    local backend=$1
    local config_name
    case $backend in
        ev)            config_name="Hello_ev.json" ;;
        native_uring)  config_name="Hello_native_uring.json" ;;
        asio_uring)    config_name="Hello_asio_uring.json" ;;
    esac

    kill $(lsof -ti :$THUNDER_PORT 2>/dev/null) 2>/dev/null || true
    sleep 1

    cd "$BENCH_DIR"
    rm -f log/*.log
    taskset -c $THUNDER_CPUS "$THUNDER_BIN" "conf/$config_name" &
    local pid=$!
    sleep 2

    # wait ready
    for i in $(seq 1 10); do
        curl -s http://127.0.0.1:$THUNDER_PORT/hello/raw > /dev/null 2>&1 && break
        sleep 1
    done

    # warmup
    taskset -c $WRK_CPUS wrk -t2 -c50 -d3s "http://${REAL_IP}:${THUNDER_PORT}/hello/raw" > /dev/null 2>&1

    for round in $(seq 1 $ROUNDS); do
        echo "  [$backend round $round]"
        taskset -c $WRK_CPUS wrk -t$THREADS -c$CONN -d${DURATION}s --latency \
            "http://${REAL_IP}:${THUNDER_PORT}/hello/raw" 2>&1 | grep -E "Requests/sec|Latency|  [57][09]%|  [9][09]%"
        echo ""
    done

    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    sleep 1
}

ensure_nginx() {
    curl -s http://127.0.0.1:$NGINX_PORT/echo > /dev/null 2>&1 || {
        nginx -c /tmp/nginx_bench.conf 2>/dev/null || true
        sleep 1
    }
}

echo "============================================="
echo "Thunder Fast-Path Benchmark — $ROUNDS rounds"
echo "Target: $REAL_IP  |  $(date)"
echo "============================================="
echo ""

for backend in ev native_uring asio_uring; do
    echo "=== $backend ==="
    run_one "$backend"
done

echo "=== Nginx ==="
ensure_nginx
# warmup
taskset -c $WRK_CPUS wrk -t2 -c50 -d3s "http://${REAL_IP}:${NGINX_PORT}/echo" > /dev/null 2>&1
for round in $(seq 1 $ROUNDS); do
    echo "  [nginx round $round]"
    taskset -c $WRK_CPUS wrk -t$THREADS -c$CONN -d${DURATION}s --latency \
        "http://${REAL_IP}:${NGINX_PORT}/echo" 2>&1 | grep -E "Requests/sec|Latency|  [57][09]%|  [9][09]%"
    echo ""
done

echo "Done."
