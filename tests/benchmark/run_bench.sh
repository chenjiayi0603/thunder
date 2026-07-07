#!/bin/bash
# Thunder I/O Backend Benchmark: libev vs io_uring for HTTP/HTTPS
set -euo pipefail

THUNDER_ROOT="/home/administrator/thunder"
DEPLOY_ROOT="$THUNDER_ROOT/deploy"
WRK="${WRK:-wrk}"
WRK_POST_LUA="$DEPLOY_ROOT/tests/benchmark/wrk_post.lua"
BENCHMARK_DIR="$DEPLOY_ROOT/tests/benchmark"
RESULTS_DIR="$BENCHMARK_DIR/results"
mkdir -p "$RESULTS_DIR"

HELLO_HTTP="$DEPLOY_ROOT/HelloHttp/bin/HelloHttp"
HELLO_HTTPS="$DEPLOY_ROOT/HelloHttps/bin/HelloHttps"
HTTP_CONF="$DEPLOY_ROOT/HelloHttp/conf/Hello.json"
HTTPS_CONF="$DEPLOY_ROOT/HelloHttps/conf/HelloHttps.json"

HTTP_PORT=27006
HTTPS_PORT=27443
DURATION="30s"
THREADS=4
CONNECTIONS_LIST="10 100 500"

# colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# check wrk
if ! command -v "$WRK" &>/dev/null; then
    echo -e "${RED}wrk not found. Install: apt install wrk${NC}"
    exit 1
fi

# wrk HTTPS check
WRK_HAS_SSL=$("$WRK" --help 2>&1 | grep -c -i "ssl\|--https" || true)

usage() {
    echo "Usage: $0 [--backend ev|asio_uring] [--backends ev,asio_uring]"
    echo "Example: $0 --backends ev,asio_uring"
    exit 1
}

BACKENDS=()
TEST_HTTP=true
TEST_HTTPS=true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend) BACKENDS+=("$2"); shift 2;;
        --backends)
            IFS=',' read -ra BACKENDS <<< "$2"
            shift 2;;
        --http-only) TEST_HTTPS=false; shift;;
        --https-only) TEST_HTTP=false; shift;;
        *) usage;;
    esac
done

if [[ ${#BACKENDS[@]} -eq 0 ]]; then
    BACKENDS=("ev")
fi

echo "============================================================"
echo " Thunder I/O Backend Benchmark"
echo " Backends: ${BACKENDS[*]}"
echo " HTTP: $TEST_HTTP, HTTPS: $TEST_HTTPS"
echo " Duration: $DURATION, Threads: $THREADS"
echo " Connections: $CONNECTIONS_LIST"
echo "============================================================"

set_backend_config() {
    local conf="$1" backend="$2"
    # Use python3 for reliable JSON editing
    python3 -c "
import json,sys
with open('$conf') as f:
    cfg=json.load(f)
cfg['io_backend']='$backend'
with open('$conf','w') as f:
    json.dump(cfg,f,indent=4,ensure_ascii=False)
"
    echo "  Set io_backend='$backend' in $(basename "$conf")"
}

start_service() {
    local bin="$1" name="$2"
    echo -n "  Starting $name..."
    cd "$(dirname "$bin")"
    "./$(basename "$bin")" -c "$3" &
    local pid=$!
    sleep 2
    if kill -0 "$pid" 2>/dev/null; then
        echo -e " ${GREEN}OK${NC} (pid $pid)"
    else
        echo -e " ${RED}FAILED${NC}"
        return 1
    fi
    echo "$pid"
}

stop_service() {
    local name="$1" pid="$2"
    echo -n "  Stopping $name (pid $pid)..."
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    sleep 1
    echo -e " ${GREEN}done${NC}"
}

run_wrk() {
    local url="$1" name="$2" conn="$3"
    local outfile="$RESULTS_DIR/${name}_c${conn}.txt"
    local extra_args=""
    if [[ "$url" == https://* ]]; then
        if [[ "$WRK_HAS_SSL" -gt 0 ]]; then
            extra_args="--no-check-certificate"
        else
            echo -e "    ${YELLOW}wrk without SSL, skipping HTTPS${NC}"
            echo "SKIP: wrk without SSL support" > "$outfile"
            return
        fi
    fi
    echo "    wrk: $name c=$conn ..." >&2
    if [[ -f "$WRK_POST_LUA" ]]; then
        "$WRK" -t"$THREADS" -c"$conn" -d"$DURATION" -s "$WRK_POST_LUA" $extra_args "$url" > "$outfile" 2>&1 || true
    else
        "$WRK" -t"$THREADS" -c"$conn" -d"$DURATION" $extra_args "$url" > "$outfile" 2>&1 || true
    fi
}

parse_wrk() {
    local file="$1"
    if grep -q "SKIP" "$file" 2>/dev/null; then
        echo "SKIP"
        return
    fi
    local rps=$(grep "Requests/sec" "$file" | awk '{print $2}' 2>/dev/null || echo "0")
    local lat=$(grep "Latency" "$file" | grep -v "Distribution" | awk '{print $2}' 2>/dev/null || echo "0")
    echo "$rps $lat"
}

# Run tests
declare -A RESULTS
SUMMARY_CSV="$RESULTS_DIR/summary.csv"
echo "backend,protocol,connections,rps,latency_ms" > "$SUMMARY_CSV"

for BACKEND in "${BACKENDS[@]}"; do
    echo ""
    echo "=== Backend: $BACKEND ==="
    
    # Build with appropriate flags
    echo "  Building..."
    BUILD_DIR="$THUNDER_ROOT/build_${BACKEND}"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    if [[ "$BACKEND" == "asio_uring" ]]; then
        cmake "$THUNDER_ROOT" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTHUNDER_IO_ASIO_URING=ON > /dev/null 2>&1
    else
        cmake "$THUNDER_ROOT" -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null 2>&1
    fi
    cmake --build . -j1 > /dev/null 2>&1
    echo "  Build complete."
    
    # Configure for this backend
    set_backend_config "$HTTP_CONF" "$BACKEND"
    set_backend_config "$HTTPS_CONF" "$BACKEND"
    
    # Start services
    HTTP_PID=""
    HTTPS_PID=""
    
    if $TEST_HTTP; then
        HTTP_PID=$(start_service "$HELLO_HTTP" "HelloHttp" "conf/Hello.json")
    fi
    if $TEST_HTTPS; then
        HTTPS_PID=$(start_service "$HELLO_HTTPS" "HelloHttps" "conf/HelloHttps.json")
    fi
    
    # Warm up
    sleep 2
    echo "  Warming up..."
    $TEST_HTTP && curl -s -X POST http://127.0.0.1:$HTTP_PORT/hello/hello -H "Content-Type: application/json" -d '{"option":"Echo","data":"worm"}' > /dev/null 2>&1 || true
    sleep 1
    
    # Run benchmarks
    for CONN in $CONNECTIONS_LIST; do
        if $TEST_HTTP; then
            echo "  [HTTP c=$CONN]"
            run_wrk "http://127.0.0.1:$HTTP_PORT/hello/hello" "http_${BACKEND}" "$CONN"
        fi
        if $TEST_HTTPS; then
            echo "  [HTTPS c=$CONN]"
            run_wrk "https://127.0.0.1:$HTTPS_PORT/hello/hello" "https_${BACKEND}" "$CONN"
        fi
    done
    
    # Stop services
    echo ""
    $TEST_HTTP && [[ -n "$HTTP_PID" ]] && stop_service "HelloHttp" "$HTTP_PID"
    $TEST_HTTPS && [[ -n "$HTTPS_PID" ]] && stop_service "HelloHttps" "$HTTPS_PID"
    
    # Parse results
    echo ""
    echo "  Results for $BACKEND:"
    echo "  ---------------------"
    for CONN in $CONNECTIONS_LIST; do
        if $TEST_HTTP; then
            HTTP_R=$(parse_wrk "$RESULTS_DIR/http_${BACKEND}_c${CONN}.txt")
            echo "  HTTP c=$CONN: $HTTP_R"
            echo "$BACKEND,http,$CONN,$HTTP_R" >> "$SUMMARY_CSV"
        fi
        if $TEST_HTTPS; then
            HTTPS_R=$(parse_wrk "$RESULTS_DIR/https_${BACKEND}_c${CONN}.txt")
            echo "  HTTPS c=$CONN: $HTTPS_R"
            echo "$BACKEND,https,$CONN,$HTTPS_R" >> "$SUMMARY_CSV"
        fi
    done
done

# Restore default config
python3 -c "
import json
for conf in ['$HTTP_CONF','$HTTPS_CONF']:
    with open(conf) as f: cfg=json.load(f)
    cfg.pop('io_backend',None)
    with open(conf,'w') as f: json.dump(cfg,f,indent=4,ensure_ascii=False)
" 2>/dev/null || true

echo ""
echo "============================================================"
echo " Benchmark Complete. Results: $RESULTS_DIR/"
echo " Summary CSV: $SUMMARY_CSV"
echo "============================================================"
