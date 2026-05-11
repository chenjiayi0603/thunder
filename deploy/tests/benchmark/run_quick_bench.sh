#!/bin/bash
# Quick Thunder benchmark: ev vs uring for HTTP/HTTPS
set -euo pipefail

THUNDER_ROOT="/home/administrator/thunder"
DEPLOY="$THUNDER_ROOT/deploy"
RESULTS="$DEPLOY/tests/benchmark/results"
mkdir -p "$RESULTS"

WRK="wrk"
DUR="10s"
THREADS=4
CONNS="10 100 500"

HTTPS_CONN="50"  # HTTPS uses fewer connections due to TLS overhead

HTTP_PORT=27006
HTTPS_PORT=27443

set_config() {
    local conf="$1" backend="$2"
    python3 -c "
import json
with open('$conf') as f: cfg=json.load(f)
cfg['io_backend']='$backend'
with open('$conf','w') as f: json.dump(cfg,f,indent=4,ensure_ascii=False)
"
}

run_test() {
    local backend="$1" protocol="$2" port="$3" conn="$4"
    local url="${protocol}://127.0.0.1:${port}/hello/hello"
    local out="$RESULTS/${protocol}_${backend}_c${conn}.txt"
    local extra=""
    if [[ "$protocol" == "https" ]]; then
        if ! "$WRK" --help 2>&1 | grep -qi ssl; then
            echo "SKIP:no_ssl" > "$out"
            return
        fi
        extra="--no-check-certificate"
    fi
    echo "  wrk $protocol $backend c=$conn ..."
    "$WRK" -t"$THREADS" -c"$conn" -d"$DUR" -s "$THUNDER_ROOT/deploy/tests/benchmark/wrk_post.lua" $extra "$url" > "$out" 2>&1 || true
}

parse_result() {
    local file="$1"
    if grep -q "SKIP" "$file" 2>/dev/null; then echo "SKIP"; return; fi
    local rps=$(grep "Requests/sec" "$file" | awk '{print $2}' 2>/dev/null || echo "0")
    local lat=$(grep "Latency" "$file" | grep -v "Distrib" | awk '{print $2}' 2>/dev/null || echo "0")
    echo "$rps $lat"
}

# Test backends
BACKENDS=("ev" "uring" "asio_uring")
SUMMARY="$RESULTS/summary.csv"
echo "backend,protocol,connections,rps,latency_ms" > "$SUMMARY"

for BACKEND in "${BACKENDS[@]}"; do
    echo ""
    echo "===== Backend: $BACKEND ====="
    
    # Build
    echo "  Building..."
    BUILD_DIR="$THUNDER_ROOT/build_${BACKEND}"
    if [[ "$BACKEND" == "uring" ]]; then
        (cd "$BUILD_DIR" && cmake --build . -j1 2>/dev/null) || {
            mkdir -p "$BUILD_DIR"
            cd "$BUILD_DIR"
            cmake "$THUNDER_ROOT" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTHUNDER_IO_URING=ON >/dev/null 2>&1
            cmake --build . -j1 >/dev/null 2>&1
        }
    elif [[ "$BACKEND" == "asio_uring" ]]; then
        (cd "$BUILD_DIR" && cmake --build . -j1 2>/dev/null) || {
            mkdir -p "$BUILD_DIR"
            cd "$BUILD_DIR"
            cmake "$THUNDER_ROOT" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTHUNDER_IO_ASIO_URING=ON >/dev/null 2>&1
            cmake --build . -j1 >/dev/null 2>&1
        }
    else
        (cd "$BUILD_DIR" && cmake --build . -j1 2>/dev/null) || true
    fi
    
    # Configure
    set_config "$DEPLOY/HelloHttp/conf/Hello.json" "$BACKEND"
    set_config "$DEPLOY/HelloHttps/conf/HelloHttps.json" "$BACKEND"
    
    # Start
    cd "$DEPLOY/HelloHttp" && ./bin/HelloHttp conf/Hello.json >/dev/null 2>&1 &
    HTTP_PID=$!
    sleep 3
    
    # Warmup
    curl -s -X POST "http://127.0.0.1:$HTTP_PORT/hello/hello" -H "Content-Type: application/json" -d '{"option":"Echo","data":"warm"}' >/dev/null 2>&1 || true
    sleep 1
    
    # HTTP benchmark
    echo "  HTTP benchmarks..."
    for CONN in $CONNS; do
        run_test "$BACKEND" "http" "$HTTP_PORT" "$CONN"
        R=$(parse_result "$RESULTS/http_${BACKEND}_c${CONN}.txt")
        echo "    HTTP c=$CONN: $R"
        echo "$BACKEND,http,$CONN,$R" >> "$SUMMARY"
    done
    
    # Stop HTTP
    kill $HTTP_PID 2>/dev/null; wait $HTTP_PID 2>/dev/null || true
    sleep 1
    
    # HTTPS benchmark
    cd "$DEPLOY/HelloHttps" && ./bin/HelloHttps conf/HelloHttps.json >/dev/null 2>&1 &
    HTTPS_PID=$!
    sleep 3
    
    echo "  HTTPS benchmarks..."
    for CONN in 10 50 100; do
        run_test "$BACKEND" "https" "$HTTPS_PORT" "$CONN"
        R=$(parse_result "$RESULTS/https_${BACKEND}_c${CONN}.txt")
        echo "    HTTPS c=$CONN: $R"
        echo "$BACKEND,https,$CONN,$R" >> "$SUMMARY"
    done
    
    kill $HTTPS_PID 2>/dev/null; wait $HTTPS_PID 2>/dev/null || true
    sleep 1
done

# Restore default configs
python3 -c "
for conf in ['$DEPLOY/HelloHttp/conf/Hello.json','$DEPLOY/HelloHttps/conf/HelloHttps.json']:
    with open(conf) as f: cfg=json.load(f)
    cfg.pop('io_backend',None)
    with open(conf,'w') as f: json.dump(cfg,f,indent=4,ensure_ascii=False)
" 2>/dev/null || true

echo ""
echo "===== RESULTS ====="
cat "$SUMMARY"
echo ""
echo "Detailed results in: $RESULTS/"
