#!/usr/bin/env bash
# Thunder I/O Backend 真实网卡性能对比测试
# 测试方法: POST 变长二进制 body (不解析) → 固定返回 24B JSON (公平对比)
# 目标: 192.168.3.61 (物理网卡 enp0s31f6)
# 对比: ev / native_uring / asio_uring / Nginx
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
THUNDER_BIN="/home/tommychen/thunder/build/bin/Hello"
REAL_IP="192.168.3.61"
THUNDER_PORT=28006
NGINX_PORT=18088
RESULT_FILE="$BENCH_DIR/results_$(date +%Y%m%d_%H%M%S).txt"
DURATION=10
WRK_THREADS=4
WRK_CONN=100

# CPU binding: Thunder on P-cores, wrk on E-cores (matches original methodology)
THUNDER_CPUS="4-9"
WRK_CPUS="12-19"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

log()  { echo -e "${GREEN}[$(date +%H:%M:%S)]${NC} $*" | tee -a "$RESULT_FILE"; }
warn() { echo -e "${YELLOW}[$(date +%H:%M:%S)]${NC} $*" | tee -a "$RESULT_FILE"; }

ensure_nginx() {
    if ! curl -s http://127.0.0.1:$NGINX_PORT/ > /dev/null 2>&1; then
        log "Starting Nginx on port $NGINX_PORT..."
        nginx -c /tmp/nginx_bench.conf 2>/dev/null || true
        sleep 1
    fi
    log "Nginx OK: $(curl -s http://127.0.0.1:$NGINX_PORT/)"
}

start_thunder() {
    local backend=$1
    local config_name
    case $backend in
        ev)            config_name="Hello_ev.json" ;;
        native_uring)  config_name="Hello_native_uring.json" ;;
        asio_uring)    config_name="Hello_asio_uring.json" ;;
        *) echo "Unknown backend: $backend"; exit 1 ;;
    esac

    # Kill any existing Thunder on our port
    kill $(lsof -ti :$THUNDER_PORT 2>/dev/null) 2>/dev/null || true
    sleep 1

    log "Starting Thunder ($backend) on :$THUNDER_PORT..."
    cd "$BENCH_DIR"
    rm -f log/*.log
    taskset -c $THUNDER_CPUS "$THUNDER_BIN" "conf/$config_name" &
    THUNDER_PID=$!
    sleep 2

    # Wait until responding
    for i in $(seq 1 10); do
        if curl -s http://127.0.0.1:$THUNDER_PORT/hello/raw > /dev/null 2>&1; then
            log "Thunder ($backend) ready (PID $THUNDER_PID)"
            return 0
        fi
        sleep 1
    done
    warn "Thunder ($backend) failed to start!"
    return 1
}

stop_thunder() {
    kill $THUNDER_PID 2>/dev/null || true
    wait $THUNDER_PID 2>/dev/null || true
    sleep 1
    log "Thunder stopped."
}

run_wrk() {
    local url=$1
    local label=$2
    local wrk_script=${3:-}

    local script_arg=""
    [ -n "$wrk_script" ] && script_arg="-s $wrk_script"

    log "  WRK: $label ($url)"
    taskset -c $WRK_CPUS wrk -t$WRK_THREADS -c$WRK_CONN -d${DURATION}s --latency $script_arg "$url" 2>&1 | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"
}

run_thunder_tests() {
    local backend=$1
    local base_url="http://${REAL_IP}:${THUNDER_PORT}"

    log ""
    log "============================================================"
    log "  Backend: $backend"
    log "============================================================"

    start_thunder "$backend" || return 1

    # Warmup
    log "Warmup..."
    taskset -c $WRK_CPUS wrk -t2 -c50 -d3s -s "$BENCH_DIR/post_64.lua" "$base_url/hello/raw" > /dev/null 2>&1

    # 公平对比: POST 变长二进制 body (不解析) → 固定 24B 响应
    run_wrk "$base_url/hello/raw" "POST 64B binary"  "$BENCH_DIR/post_64.lua"
    run_wrk "$base_url/hello/raw" "POST 1K binary"   "$BENCH_DIR/post_1k.lua"
    run_wrk "$base_url/hello/raw" "POST 4K binary"   "$BENCH_DIR/post_4k.lua"
    run_wrk "$base_url/hello/raw" "POST 16K binary"  "$BENCH_DIR/post_16k.lua"
    run_wrk "$base_url/hello/raw" "POST 64K binary"  "$BENCH_DIR/post_64k.lua"

    # Thunder-only: Echo (JSON解析+动态构造)
    run_wrk "$base_url/hello/hello" "Echo 64B"  "$BENCH_DIR/wrk_echo_64.lua"
    run_wrk "$base_url/hello/hello" "Echo 4K"   "$BENCH_DIR/wrk_echo_4k.lua"
    run_wrk "$base_url/hello/hello" "Echo 64K"  "$BENCH_DIR/wrk_echo_64k.lua"

    stop_thunder
}

run_nginx_tests() {
    local base_url="http://${REAL_IP}:${NGINX_PORT}"

    log ""
    log "============================================================"
    log "  Nginx Baseline (1 worker, epoll, access_log off)"
    log "============================================================"

    ensure_nginx

    # Warmup
    taskset -c $WRK_CPUS wrk -t2 -c50 -d3s -s "$BENCH_DIR/post_64.lua" "$base_url/" > /dev/null 2>&1

    # 公平对比: POST 变长二进制 (不解析) → return 200 固定 24B JSON
    run_wrk "$base_url/" "POST 64B binary"  "$BENCH_DIR/post_64.lua"
    run_wrk "$base_url/" "POST 1K binary"   "$BENCH_DIR/post_1k.lua"
    run_wrk "$base_url/" "POST 4K binary"   "$BENCH_DIR/post_4k.lua"
    run_wrk "$base_url/" "POST 16K binary"  "$BENCH_DIR/post_16k.lua"
    run_wrk "$base_url/" "POST 64K binary"  "$BENCH_DIR/post_64k.lua"
}

# =============================================================
# Main
# =============================================================
echo "Thunder I/O Backend Benchmark — Real NIC ($REAL_IP)" | tee "$RESULT_FILE"
echo "Date: $(date)" | tee -a "$RESULT_FILE"
echo "CPU: $(lscpu | grep 'Model name' | cut -d: -f2 | xargs)" | tee -a "$RESULT_FILE"
echo "NIC: $(ethtool enp0s31f6 2>/dev/null | grep -E 'Speed|Duplex|Link' || echo 'Link: up')" | tee -a "$RESULT_FILE"
echo "Governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)" | tee -a "$RESULT_FILE"
echo "Thunder cores: $THUNDER_CPUS | wrk cores: $WRK_CPUS" | tee -a "$RESULT_FILE"
echo "Duration: ${DURATION}s, Threads: $WRK_THREADS, Connections: $WRK_CONN" | tee -a "$RESULT_FILE"
echo "" | tee -a "$RESULT_FILE"

# Run tests
run_thunder_tests "ev"
run_thunder_tests "native_uring"
run_thunder_tests "asio_uring"
run_nginx_tests

log ""
log "============================================================"
log "Benchmark complete. Results: $RESULT_FILE"
log "============================================================"
