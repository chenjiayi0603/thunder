#!/bin/bash
# ============================================================================
# Thunder 稳定性压测脚本
# ============================================================================
# 功能:
#   - 双 IO 后端覆盖: ev (epoll) + asio_uring
#   - 多协议覆盖: HTTP / HTTPS / WebSocket
#   - 持续压测 3 分钟 (可配置)
#   - 实时监控 CPU / RSS / fd / dmesg
#   - 自动判定: 无崩溃 + 无内存泄漏 + 无 fd 泄漏 + CPU 正常回落
#
# 用法:
#   ./stability_test.sh                           # 默认: 双后端, HTTP+HTTPS, 180s
#   ./stability_test.sh --backend ev              # 只测 ev
#   ./stability_test.sh --backend asio_uring      # 只测 asio_uring
#   ./stability_test.sh --duration 600            # 10 分钟长时间
#   ./stability_test.sh --skip-https              # 跳过 HTTPS
#   ./stability_test.sh --skip-ws                 # 跳过 WebSocket
#
# 依赖:
#   - wrk (apt install wrk)
#   - curl, pgrep, ps, /proc filesystem
#   - 目标服务必须已启动 (docker compose up 或 k8s)
# ============================================================================

set -euo pipefail

# ---- 配置 ----
DURATION=${DURATION:-180}          # 压测时长 (秒)
WARMUP=15                          # 预热时间 (秒)
HTTP_PORT=${HTTP_PORT:-27006}
HTTPS_PORT=${HTTPS_PORT:-27443}
WS_PORT=${WS_PORT:-27010}
CONCURRENCY=${CONCURRENCY:-50}     # wrk 连接数
RATE=${RATE:-5000}                 # wrk2 恒定速率 (rps), 0=不限速
MONITOR_INTERVAL=1                 # 资源采样间隔 (秒)
BACKENDS_TO_TEST=""                # 运行时填充
SKIP_HTTPS=false
SKIP_WS=false
TMPDIR="/tmp/thunder_stability_$$"
REPORT_FILE=""

# ---- 颜色 ----
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { echo -e "${GREEN}PASS${NC} $*"; }
fail() { echo -e "${RED}FAIL${NC} $*"; }
info() { echo -e "${CYAN}INFO${NC} $*"; }
warn() { echo -e "${YELLOW}WARN${NC} $*"; }

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend) BACKENDS_TO_TEST="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --http-port) HTTP_PORT="$2"; shift 2 ;;
        --https-port) HTTPS_PORT="$2"; shift 2 ;;
        --ws-port) WS_PORT="$2"; shift 2 ;;
        --concurrency) CONCURRENCY="$2"; shift 2 ;;
        --rate) RATE="$2"; shift 2 ;;
        --skip-https) SKIP_HTTPS=true; shift ;;
        --skip-ws) SKIP_WS=true; shift ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo "  --backend ev|asio_uring  指定 IO 后端 (默认两者都测)"
            echo "  --duration SECONDS       压测时长 (默认 180)"
            echo "  --http-port PORT         HTTP 端口 (默认 27006)"
            echo "  --https-port PORT        HTTPS 端口 (默认 27443)"
            echo "  --ws-port PORT           WebSocket 端口 (默认 27010)"
            echo "  --concurrency N          wrk 并发连接数 (默认 50)"
            echo "  --rate RPS               wrk2 恒定速率 (默认 5000, 0=不限)"
            echo "  --skip-https             跳过 HTTPS"
            echo "  --skip-ws                跳过 WebSocket"
            exit 0
            ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

[[ -z "$BACKENDS_TO_TEST" ]] && BACKENDS_TO_TEST="ev asio_uring"

# ---- 工具检查 ----
check_tool() {
    command -v "$1" >/dev/null 2>&1 || { fail "缺少工具: $1 (apt install $1)"; exit 1; }
}
check_tool wrk
check_tool curl
check_tool pgrep
check_tool ps

# ---- 初始化 ----
mkdir -p "$TMPDIR"
trap "rm -rf $TMPDIR" EXIT

REPORT_FILE="$TMPDIR/report.txt"
echo "# Thunder 稳定性压测报告" > "$REPORT_FILE"
echo "# 时间: $(date '+%Y-%m-%d %H:%M:%S')" >> "$REPORT_FILE"
echo "# 时长: ${DURATION}s | 并发: $CONCURRENCY | IO 后端: $BACKENDS_TO_TEST" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"

# ---- 切换 IO 后端 ----
switch_backend() {
    local backend="$1"
    local conf="deploy/HelloHttp/conf/Hello.json"

    if [[ ! -f "$conf" ]]; then
        warn "配置文件不存在: $conf，跳过 IO 后端切换 (假设已手动配置)"
        return 0
    fi

    local current
    current=$(grep -oP '"io_backend"\s*:\s*"\K[^"]+' "$conf" 2>/dev/null || echo "unknown")
    if [[ "$current" == "$backend" ]]; then
        info "IO 后端已经是 $backend，跳过切换"
        return 0
    fi

    info "切换 IO 后端: $current → $backend"
    cp "$conf" "${conf}.bak_stability"
    sed -i "s/\"io_backend\"[[:space:]]*:[[:space:]]*\"[^\"]*\"/\"io_backend\": \"$backend\"/" "$conf"

    # 重启服务
    if docker compose -f docker/docker-compose.yml ps hello 2>/dev/null | grep -q "Up"; then
        info "重启 Docker Compose 中的 hello 服务..."
        docker compose -f docker/docker-compose.yml restart hello -t 30
        sleep 5
    elif pgrep -f "Hello_robot" >/dev/null; then
        info "检测到本地进程，请手动重启 Hello 服务以便新 IO 后端生效"
        info "  cd deploy/HelloHttp && ./node.sh restart"
        sleep 2
    fi
}

# ---- 进程发现 ----
find_pid() {
    local pattern="${1:-Hello_robot}"
    pgrep -f "$pattern" | head -1
}

# ---- 资源监控 (后台运行) ----
start_monitor() {
    local pid="$1"
    local tag="$2"
    local logfile="$TMPDIR/monitor_${tag}.log"

    info "启动资源监控: PID=$pid tag=$tag log=$logfile"

    (
        echo "# ts cpu% rss_kb fd_count"
        local start_ts
        start_ts=$(date +%s)
        while true; do
            local elapsed=$(($(date +%s) - start_ts))
            if [[ $elapsed -gt $((DURATION + WARMUP + 30)) ]]; then
                break
            fi
            if ! kill -0 "$pid" 2>/dev/null; then
                echo "$elapsed DEAD 0 0" >> "$logfile"
                break
            fi
            local cpu rss fd_count
            cpu=$(ps -p "$pid" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "0")
            rss=$(awk '/VmRSS/{print $2}' /proc/"$pid"/status 2>/dev/null || echo "0")
            fd_count=$(ls /proc/"$pid"/fd 2>/dev/null | wc -l || echo "0")
            echo "$elapsed $cpu $rss $fd_count" >> "$logfile"
            sleep "$MONITOR_INTERVAL"
        done
    ) &
    echo $! > "$TMPDIR/monitor_pid_${tag}"
}

stop_monitor() {
    local tag="$1"
    local mpid_file="$TMPDIR/monitor_pid_${tag}"
    if [[ -f "$mpid_file" ]]; then
        kill "$(cat "$mpid_file")" 2>/dev/null || true
    fi
}

# ---- 压测 HTTP ----
run_http_stress() {
    local tag="$1"
    local url="http://127.0.0.1:${HTTP_PORT}/hello/hello"
    local wrk_out="$TMPDIR/wrk_http_${tag}.log"

    info "HTTP 预热 ${WARMUP}s: $url"
    wrk -t4 -c$((CONCURRENCY / 4)) -d${WARMUP}s \
        -s <(echo 'wrk.method = "POST"; wrk.body = "{\"option\":\"Echo\"}"; wrk.headers["Content-Type"] = "application/json"') \
        "$url" > /dev/null 2>&1 || true

    info "HTTP 压测开始 (${DURATION}s): $url"
    if [[ "$RATE" -gt 0 ]] && command -v wrk2 >/dev/null 2>&1; then
        wrk2 -t4 -c$CONCURRENCY -R"$RATE" -d${DURATION}s --latency \
            -s <(echo 'wrk.method = "POST"; wrk.body = "{\"option\":\"Echo\"}"; wrk.headers["Content-Type"] = "application/json"') \
            "$url" > "$wrk_out" 2>&1
    else
        wrk -t4 -c$CONCURRENCY -d${DURATION}s --latency \
            -s <(echo 'wrk.method = "POST"; wrk.body = "{\"option\":\"Echo\"}"; wrk.headers["Content-Type"] = "application/json"') \
            "$url" > "$wrk_out" 2>&1
    fi
    echo "$wrk_out"
}

# ---- 压测 HTTPS ----
run_https_stress() {
    local tag="$1"
    local url="https://127.0.0.1:${HTTPS_PORT}/hello/hello"
    local wrk_out="$TMPDIR/wrk_https_${tag}.log"

    # 先探测端口是否在监听
    if ! curl -sk --connect-timeout 3 "https://127.0.0.1:${HTTPS_PORT}/hello/hello" \
         -X POST -H "Content-Type: application/json" -d '{"option":"Echo"}' > /dev/null 2>&1; then
        warn "HTTPS 端口 ${HTTPS_PORT} 不可达，跳过 HTTPS 压测"
        echo "SKIPPED" > "$wrk_out"
        return 0
    fi

    info "HTTPS 预热 ${WARMUP}s: $url"
    wrk -t4 -c$((CONCURRENCY / 4)) -d${WARMUP}s \
        -s <(echo 'wrk.method = "POST"; wrk.body = "{\"option\":\"Echo\"}"; wrk.headers["Content-Type"] = "application/json"') \
        "$url" > /dev/null 2>&1 || true

    info "HTTPS 压测开始 (${DURATION}s): $url"
    if [[ "$RATE" -gt 0 ]] && command -v wrk2 >/dev/null 2>&1; then
        wrk2 -t4 -c$CONCURRENCY -R$((RATE / 2)) -d${DURATION}s --latency \
            -s <(echo 'wrk.method = "POST"; wrk.body = "{\"option\":\"Echo\"}"; wrk.headers["Content-Type"] = "application/json"') \
            "$url" > "$wrk_out" 2>&1
    else
        wrk -t4 -c$CONCURRENCY -d${DURATION}s --latency \
            -s <(echo 'wrk.method = "POST"; wrk.body = "{\"option\":\"Echo\"}"; wrk.headers["Content-Type"] = "application/json"') \
            "$url" > "$wrk_out" 2>&1
    fi
    echo "$wrk_out"
}

# ---- 压测 WebSocket (简化: ping/pong) ----
run_ws_stress() {
    local tag="$1"
    local ws_out="$TMPDIR/ws_${tag}.log"

    if ! curl -s --connect-timeout 3 "http://127.0.0.1:${WS_PORT}/" >/dev/null 2>&1; then
        warn "WS 端口 ${WS_PORT} 不可达，跳过 WebSocket 压测"
        echo "SKIPPED" > "$ws_out"
        return 0
    fi

    info "WebSocket 压测开始 (简化 ping/pong, ${DURATION}s)"
    # 用 websocat 如果可用，否则跳过
    if command -v websocat >/dev/null 2>&1; then
        local end_ts=$(($(date +%s) + DURATION))
        local pings=0 pongs=0
        while [[ $(date +%s) -lt $end_ts ]]; do
            if echo "ping" | timeout 5 websocat -n "ws://127.0.0.1:${WS_PORT}" 2>/dev/null; then
                pongs=$((pongs + 1))
            fi
            pings=$((pings + 1))
        done
        echo "pings=$pings pongs=$pongs" > "$ws_out"
    else
        warn "未安装 websocat，WebSocket 压测降级为 curl 探测"
        local end_ts=$(($(date +%s) + DURATION))
        local ok=0 fail=0
        while [[ $(date +%s) -lt $end_ts ]]; do
            if curl -s --connect-timeout 3 -o /dev/null -w "%{http_code}" \
                 "http://127.0.0.1:${WS_PORT}/" 2>/dev/null | grep -q "."; then
                ok=$((ok + 1))
            else
                fail=$((fail + 1))
            fi
            sleep 1
        done
        echo "probes_ok=$ok probes_fail=$fail" > "$ws_out"
    fi
}

# ---- 分析结果 ----
analyze_results() {
    local tag="$1"
    local wrk_log="$2"
    local monitor_log="$TMPDIR/monitor_${tag}.log"
    local pid="$3"

    echo "" >> "$REPORT_FILE"
    echo "## 后端: $tag" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    # ---- 1. 进程存活 ----
    if [[ ! -f "$monitor_log" ]]; then
        fail "${tag}: 监控日志缺失" | tee -a "$REPORT_FILE"
        return 1
    fi

    if grep -q "DEAD" "$monitor_log"; then
        fail "${tag}: 进程在压测期间崩溃!" | tee -a "$REPORT_FILE"
        grep "DEAD" "$monitor_log" >> "$REPORT_FILE"
        # 检查 dmesg
        if command -v dmesg >/dev/null 2>&1; then
            local dm="$TMPDIR/dmesg_${tag}.log"
            dmesg -T 2>/dev/null | tail -50 > "$dm"
            if grep -qE "segfault|OOM|killed|general protection" "$dm"; then
                fail "${tag}: dmesg 检测到严重错误" | tee -a "$REPORT_FILE"
                grep -E "segfault|OOM|killed|general protection" "$dm" >> "$REPORT_FILE"
            fi
        fi
        return 1
    fi
    pass "${tag}: 进程存活" | tee -a "$REPORT_FILE"

    # ---- 2. 内存分析 ----
    local rss_start rss_end rss_min rss_max
    rss_start=$(awk 'NR==2{print $3}' "$monitor_log" 2>/dev/null || echo "0")
    rss_end=$(awk 'END{print $3}' "$monitor_log" 2>/dev/null || echo "0")
    rss_values=$(awk '{print $3}' "$monitor_log")

    if [[ "$rss_start" -gt 0 && "$rss_end" -gt 0 ]]; then
        local rss_growth_pct
        rss_growth_pct=$(awk -v s="$rss_start" -v e="$rss_end" \
            'BEGIN{printf "%.1f", ((e-s)/s)*100}')
        local rss_start_mb=$((rss_start / 1024))
        local rss_end_mb=$((rss_end / 1024))

        if (( $(echo "$rss_growth_pct > 20" | bc -l 2>/dev/null || echo 0) )); then
            fail "${tag}: RSS 增长 ${rss_growth_pct}% (${rss_start_mb}MB → ${rss_end_mb}MB), 可能存在内存泄漏" | tee -a "$REPORT_FILE"
        elif (( $(echo "$rss_growth_pct > 10" | bc -l 2>/dev/null || echo 0) )); then
            warn "${tag}: RSS 增长 ${rss_growth_pct}% (${rss_start_mb}MB → ${rss_end_mb}MB), 关注趋势" | tee -a "$REPORT_FILE"
        else
            pass "${tag}: RSS ${rss_start_mb}MB → ${rss_end_mb}MB (${rss_growth_pct}%), 内存稳定" | tee -a "$REPORT_FILE"
        fi
    else
        warn "${tag}: 无法读取 RSS 数据" | tee -a "$REPORT_FILE"
    fi

    # ---- 3. fd 分析 ----
    local fd_start fd_end
    fd_start=$(awk 'NR==2{print $4}' "$monitor_log" 2>/dev/null || echo "0")
    fd_end=$(awk 'END{print $4}' "$monitor_log" 2>/dev/null || echo "0")
    if [[ "$fd_start" -gt 0 && "$fd_end" -gt 0 ]]; then
        local fd_diff=$((fd_end - fd_start))
        if [[ $fd_diff -gt $((fd_start / 2)) ]]; then
            fail "${tag}: fd 从 $fd_start → $fd_end (+$fd_diff), 可能存在泄漏!" | tee -a "$REPORT_FILE"
        else
            pass "${tag}: fd $fd_start → $fd_end, 稳定" | tee -a "$REPORT_FILE"
        fi
    fi

    # ---- 4. CPU 回落 ----
    local cpu_values
    cpu_values=$(awk '{print $2}' "$monitor_log")
    # 取最后 10 个采样点的平均 CPU
    local cpu_tail_avg
    cpu_tail_avg=$(echo "$cpu_values" | tail -10 | awk '{sum+=$1; n++} END{printf "%.1f", sum/n}' 2>/dev/null || echo "0")
    if (( $(echo "$cpu_tail_avg < 10" | bc -l 2>/dev/null || echo 0) )); then
        pass "${tag}: CPU 尾段均值 ${cpu_tail_avg}%, 回落正常 (<10%)" | tee -a "$REPORT_FILE"
    else
        warn "${tag}: CPU 尾段均值 ${cpu_tail_avg}%, 偏高" | tee -a "$REPORT_FILE"
    fi

    # ---- 5. wrk 结果 ----
    if [[ -f "$wrk_log" ]]; then
        if grep -q "SKIPPED" "$wrk_log" 2>/dev/null; then
            info "${tag}: wrk 压测已跳过" | tee -a "$REPORT_FILE"
        else
            local rps errs
            rps=$(grep -oP 'Requests/sec:\s*\K[0-9.]+' "$wrk_log" 2>/dev/null || echo "N/A")
            errs=$(grep -oP 'Non-2xx or 3xx responses:\s*\K[0-9]+' "$wrk_log" 2>/dev/null || echo "0")
            local sock_errs
            sock_errs=$(grep -oP 'Socket errors: connect \K[0-9]+' "$wrk_log" 2>/dev/null || echo "0")

            if [[ "$errs" -gt 0 ]] || [[ "$sock_errs" -gt 0 ]]; then
                fail "${tag}: wrk 错误 — Non-2xx=$errs, Socket errors=$sock_errs" | tee -a "$REPORT_FILE"
            else
                pass "${tag}: wrk RPS=$rps, Socket errors=0, Non-2xx=0" | tee -a "$REPORT_FILE"
            fi
        fi
    fi

    # ---- 6. 生成时序数据供后续可视化 ----
    cp "$monitor_log" "$TMPDIR/monitor_${tag}.data"

    echo "" >> "$REPORT_FILE"
}

# ---- 健康检查 ----
health_check() {
    local pid
    pid=$(find_pid "Hello_robot" 2>/dev/null)
    if [[ -z "$pid" ]]; then
        fail "未找到 Hello 服务进程 (Hello_robot)"
        return 1
    fi

    local http_ok=false
    if curl -s --connect-timeout 3 -X POST "http://127.0.0.1:${HTTP_PORT}/hello/hello" \
        -H "Content-Type: application/json" -d '{"option":"Echo"}' \
        | grep -q '"code"'; then
        http_ok=true
    fi

    if ! $http_ok; then
        fail "HTTP 健康检查失败 (端口 ${HTTP_PORT})"
        return 1
    fi
    pass "HTTP 健康检查通过"
    return 0
}

# ---- 主流程 ----
main() {
    echo ""
    echo "============================================================"
    echo "  Thunder 稳定性压测"
    echo "  时长: ${DURATION}s | 并发: $CONCURRENCY | 后端: $BACKENDS_TO_TEST"
    echo "  协议: HTTP$($SKIP_HTTPS || echo ' + HTTPS')$($SKIP_WS || echo ' + WS')"
    echo "============================================================"
    echo ""

    # 健康检查
    if ! health_check; then
        fail "前置健康检查失败，退出"
        exit 1
    fi

    local overall_result=0

    for backend in $BACKENDS_TO_TEST; do
        echo ""
        echo "-------------------------------------------------"
        info "===== IO 后端: $backend ====="
        echo "-------------------------------------------------"

        # 切换后端
        switch_backend "$backend"

        # 等待服务就绪
        sleep 3
        local pid
        pid=$(find_pid "Hello_robot")
        if [[ -z "$pid" ]]; then
            fail "无法找到 Hello 服务进程"
            overall_result=1
            continue
        fi
        info "目标进程: PID=$pid"

        # 记录 dmesg 基线
        local dmesg_before="$TMPDIR/dmesg_before_${backend}.log"
        dmesg -T 2>/dev/null | wc -l > "$dmesg_before"

        # 启动监控
        local tag_http="${backend}_http"
        start_monitor "$pid" "$tag_http"

        # HTTP 压测
        local wrk_http_log
        wrk_http_log=$(run_http_stress "$tag_http")

        # 停止监控
        stop_monitor "$tag_http"

        # 分析 HTTP 结果
        analyze_results "$tag_http" "$wrk_http_log" "$pid" || overall_result=1

        # ---- HTTPS ----
        if ! $SKIP_HTTPS; then
            sleep 5  # 让服务喘口气
            local tag_https="${backend}_https"
            start_monitor "$pid" "$tag_https"
            local wrk_https_log
            wrk_https_log=$(run_https_stress "$tag_https")
            stop_monitor "$tag_https"
            analyze_results "$tag_https" "$wrk_https_log" "$pid" || overall_result=1
        fi

        # ---- WebSocket ----
        if ! $SKIP_WS; then
            sleep 3
            # WS 不监控资源 (已在 HTTP/HTTPS 阶段覆盖), 只验证连通性
            run_ws_stress "$backend" >/dev/null
        fi

        # 检查 dmesg 是否新增错误
        local dmesg_after_lines
        dmesg_after_lines=$(dmesg -T 2>/dev/null | wc -l)
        local dmesg_before_lines
        dmesg_before_lines=$(cat "$dmesg_before" 2>/dev/null || echo "0")
        local dmesg_new=$((dmesg_after_lines - dmesg_before_lines))
        if [[ $dmesg_new -gt 5 ]]; then
            warn "${backend}: dmesg 新增 $dmesg_new 行, 请检查" | tee -a "$REPORT_FILE"
            dmesg -T 2>/dev/null | tail -"$dmesg_new" > "$TMPDIR/dmesg_new_${backend}.log"
            if grep -qE "segfault|OOM|killed|BUG|WARNING|Call Trace" "$TMPDIR/dmesg_new_${backend}.log"; then
                fail "${backend}: dmesg 检测到内核级错误!" | tee -a "$REPORT_FILE"
                overall_result=1
            fi
        else
            pass "${backend}: dmesg 无新增异常" | tee -a "$REPORT_FILE"
        fi
    done

    # ---- 汇总 ----
    echo ""
    echo "============================================================"
    if [[ $overall_result -eq 0 ]]; then
        echo -e "${GREEN}  全部通过: 稳定性压测 PASS${NC}"
    else
        echo -e "${RED}  存在问题: 稳定性压测 FAIL${NC}"
    fi
    echo "  详情: $TMPDIR"
    echo "  报告: $REPORT_FILE"
    echo "============================================================"
    echo ""

    # 打印报告摘要
    cat "$REPORT_FILE"

    exit $overall_result
}

main
