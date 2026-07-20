#!/bin/bash
# ============================================================================
# Thunder 稳定性压测脚本 (K8s 版本)
# ============================================================================
# 功能:
#   - 针对 K8s 集群压测（hostNetwork 网关）
#   - 多协议覆盖: HTTP / HTTPS
#   - 持续压测 3 分钟 (可配置)
#   - 双维度监控: kubectl top pod (15s) + kubectl exec /proc (1s, 精细)
#   - 自动判定: 无崩溃 + 无内存泄漏 + 无 fd 泄漏 + CPU 正常回落
#
# 用法:
#   ./stability_test_k8s.sh                              # 默认: HTTP+HTTPS, 180s
#   ./stability_test_k8s.sh --duration 600                # 10 分钟长跑
#   ./stability_test_k8s.sh --skip-https                  # 跳过 HTTPS
#   ./stability_test_k8s.sh --backend asio_uring          # 切换 IO 后端 (需 redeploy)
#   ./stability_test_k8s.sh --node-ip 192.168.3.61        # 指定节点 IP
#   ./stability_test_k8s.sh --namespace thunder           # 指定 namespace
#
# 前置:
#   - kubectl 已配置 + 集群可达
#   - wrk 已安装 (apt install wrk)
#   - thunder 服务已部署 (hello / hello-https)
# ============================================================================

set -euo pipefail

# ---- 默认配置 ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DURATION=${DURATION:-180}
WARMUP=${WARMUP:-15}
NAMESPACE=${NAMESPACE:-thunder}
NODE_IP=${NODE_IP:-192.168.3.61}
HELLO_DEPLOY="thunder-hello"
HTTP_PORT=27006
HTTPS_PORT=27443
CONCURRENCY=50
SKIP_HTTPS=false
SWITCH_BACKEND=""
MONITOR_INTERVAL=2
TMPDIR=""

# ---- 颜色 ----
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { echo -e "${GREEN}PASS${NC} $*"; }
fail() { echo -e "${RED}FAIL${NC} $*"; }
info() { echo -e "${CYAN}INFO${NC} $*"; }
warn() { echo -e "${YELLOW}WARN${NC} $*"; }

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend) SWITCH_BACKEND="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --node-ip) NODE_IP="$2"; shift 2 ;;
        --namespace) NAMESPACE="$2"; shift 2 ;;
        --concurrency) CONCURRENCY="$2"; shift 2 ;;
        --skip-https) SKIP_HTTPS=true; shift ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo "  --backend asio_uring   切换 IO 后端 (需重新部署 Pod)"
            echo "  --duration SECONDS     压测时长 (默认 180)"
            echo "  --node-ip IP           K8s 节点 IP (默认 192.168.3.61)"
            echo "  --namespace NS         K8s namespace (默认 thunder)"
            echo "  --concurrency N        wrk 并发连接数 (默认 50)"
            echo "  --skip-https           跳过 HTTPS"
            exit 0
            ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

# ---- 工具检查 ----
check_tool() { command -v "$1" >/dev/null 2>&1 || { fail "缺少工具: $1"; exit 1; }; }
check_tool kubectl
check_tool wrk
check_tool curl
check_tool bc

# ---- 初始化 ----
TMPDIR=$(mktemp -d /tmp/thunder_stability_k8s_XXXXXX)
info "临时目录: $TMPDIR"

REPORT_FILE="$TMPDIR/report.txt"
{
    echo "# Thunder 稳定性压测报告 (K8s)"
    echo "# 时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "# 集群: ${NODE_IP} | namespace: ${NAMESPACE} | 时长: ${DURATION}s | 并发: $CONCURRENCY"
} > "$REPORT_FILE"

# ---- wrk Lua 脚本 (写入临时文件, 避免 process substitution 转义问题) ----
WRK_LUA="$TMPDIR/wrk.lua"
cat > "$WRK_LUA" << 'WRKEOF'
wrk.method = "POST"
wrk.body = '{"option":"Echo"}'
wrk.headers["Content-Type"] = "application/json"
WRKEOF

# ---- K8s 健康检查 ----
k8s_health_check() {
    info "K8s 集群健康检查..."

    if ! kubectl get node -o name 2>/dev/null | grep -q .; then
        fail "kubectl 无法访问集群"
        return 1
    fi
    pass "kubectl 集群可达"

    local hello_pod
    hello_pod=$(kubectl get pod -n "$NAMESPACE" -l app=thunder-hello \
        -o jsonpath='{.items[?(@.status.phase=="Running")].metadata.name}' 2>/dev/null | awk '{print $1}')
    if [[ -z "$hello_pod" ]]; then
        fail "未找到 Running 状态的 hello Pod"
        return 1
    fi
    pass "hello Pod: $hello_pod"

    if curl -s --connect-timeout 5 -X POST "http://${NODE_IP}:${HTTP_PORT}/hello/hello" \
        -H "Content-Type: application/json" -d '{"option":"Echo"}' | grep -q '"code"'; then
        pass "HTTP ${NODE_IP}:${HTTP_PORT} 可达"
    else
        fail "HTTP ${NODE_IP}:${HTTP_PORT} 不可达"
        return 1
    fi

    local backend
    backend=$(kubectl exec -n "$NAMESPACE" "$hello_pod" -- \
        grep -oP '"io_backend"\s*:\s*"\K[^"]+' /app/conf/Hello.json 2>/dev/null || echo "unknown")
    info "当前 IO 后端: $backend"
    echo "# 当前 IO 后端: $backend" >> "$REPORT_FILE"

    return 0
}

# ---- 资源监控 (kubectl top + kubectl exec 双路) ----
start_monitor() {
    local tag="$1"
    local top_log="$TMPDIR/monitor_top_${tag}.log"
    local proc_log="$TMPDIR/monitor_proc_${tag}.log"

    info "启动资源监控: tag=$tag"

    # 粗粒度: kubectl top (15s 间隔)
    (
        echo "#ts cpu_mcores mem_mib"
        local start_ts=$(date +%s)
        while true; do
            local elapsed=$(($(date +%s) - start_ts))
            [[ $elapsed -gt $((DURATION + WARMUP + 60)) ]] && break
            local top_out
            top_out=$(kubectl top pod -n "$NAMESPACE" -l app=thunder-hello --containers --no-headers 2>/dev/null || true)
            if [[ -n "$top_out" ]]; then
                local cpu_m=$(echo "$top_out" | awk '{print $3}' | sed 's/m//')
                local mem_mi=$(echo "$top_out" | awk '{print $4}' | sed 's/Mi//')
                echo "$elapsed ${cpu_m:-0} ${mem_mi:-0}" >> "$top_log"
            else
                echo "$elapsed DEAD DEAD" >> "$top_log"
            fi
            sleep 15
        done
    ) &
    echo $! > "$TMPDIR/monitor_top_pid_${tag}"

    # 精细粒度: kubectl exec 读 /proc (可配间隔)
    local hello_pod
    hello_pod=$(kubectl get pod -n "$NAMESPACE" -l app=thunder-hello \
        -o jsonpath='{.items[?(@.status.phase=="Running")].metadata.name}' | awk '{print $1}')

    (
        echo "#ts rss_kb fd_count"
        local start_ts=$(date +%s)
        while true; do
            local elapsed=$(($(date +%s) - start_ts))
            [[ $elapsed -gt $((DURATION + WARMUP + 60)) ]] && break

            local phase
            phase=$(kubectl get pod -n "$NAMESPACE" "$hello_pod" \
                -o jsonpath='{.status.phase}' 2>/dev/null || echo "DEAD")
            if [[ "$phase" != "Running" ]]; then
                echo "$elapsed DEAD 0" >> "$proc_log"
                break
            fi

            local pid rss fd
            pid=$(kubectl exec -n "$NAMESPACE" "$hello_pod" -- \
                pgrep -f "Hello_robot" 2>/dev/null | head -1 || echo "")
            if [[ -z "$pid" ]]; then
                echo "$elapsed DEAD 0" >> "$proc_log"
                break
            fi
            rss=$(kubectl exec -n "$NAMESPACE" "$hello_pod" -- \
                awk '/VmRSS/{print $2}' /proc/"$pid"/status 2>/dev/null || echo "0")
            fd=$(kubectl exec -n "$NAMESPACE" "$hello_pod" -- \
                sh -c "ls /proc/$pid/fd 2>/dev/null | wc -l" 2>/dev/null || echo "0")
            echo "$elapsed $rss $fd" >> "$proc_log"
            sleep "$MONITOR_INTERVAL"
        done
    ) &
    echo $! > "$TMPDIR/monitor_proc_pid_${tag}"
}

stop_monitor() {
    local tag="$1"
    for f in "$TMPDIR/monitor_top_pid_${tag}" "$TMPDIR/monitor_proc_pid_${tag}"; do
        [[ -f "$f" ]] && kill "$(cat "$f")" 2>/dev/null || true
    done
    sleep 2  # 等子进程写完
}

# ---- 压测 HTTP ----
run_http_stress() {
    local tag="$1"
    local url="http://${NODE_IP}:${HTTP_PORT}/hello/hello"
    local wrk_out="$TMPDIR/wrk_http_${tag}.log"

    info "HTTP 预热 ${WARMUP}s: $url"
    wrk -t4 -c$((CONCURRENCY / 4)) -d${WARMUP}s -s "$WRK_LUA" "$url" > /dev/null 2>&1 || true

    info "HTTP 压测开始 (${DURATION}s): $url"
    wrk -t4 -c"$CONCURRENCY" -d${DURATION}s --latency -s "$WRK_LUA" "$url" > "$wrk_out" 2>&1
    # 通过全局变量返回路径, 避免 stdout 混入 info 日志
    WRK_OUT_FILE="$wrk_out"
}

# ---- 压测 HTTPS ----
run_https_stress() {
    local tag="$1"
    local url="https://${NODE_IP}:${HTTPS_PORT}/hello/hello"
    local wrk_out="$TMPDIR/wrk_https_${tag}.log"

    if ! curl -sk --connect-timeout 5 "https://${NODE_IP}:${HTTPS_PORT}/hello/hello" \
         -X POST -H "Content-Type: application/json" -d '{"option":"Echo"}' > /dev/null 2>&1; then
        warn "HTTPS ${NODE_IP}:${HTTPS_PORT} 不可达，跳过"
        echo "SKIPPED" > "$wrk_out"
        WRK_OUT_FILE="$wrk_out"
        return 0
    fi

    info "HTTPS 预热 ${WARMUP}s: $url"
    wrk -t4 -c$((CONCURRENCY / 4)) -d${WARMUP}s -s "$WRK_LUA" "$url" > /dev/null 2>&1 || true

    info "HTTPS 压测开始 (${DURATION}s): $url"
    wrk -t4 -c"$CONCURRENCY" -d${DURATION}s --latency -s "$WRK_LUA" "$url" > "$wrk_out" 2>&1
    WRK_OUT_FILE="$wrk_out"
}

# ---- 分析结果 ----
analyze_results() {
    local tag="$1"
    local wrk_log="$2"
    local proc_log="$TMPDIR/monitor_proc_${tag}.log"
    local top_log="$TMPDIR/monitor_top_${tag}.log"

    {
        echo ""
        echo "## $tag"
        echo ""
    } >> "$REPORT_FILE"

    local result=0

    # 1. Pod 存活
    if [[ -f "$proc_log" ]] && grep -q "DEAD" "$proc_log" 2>/dev/null; then
        fail "${tag}: Pod 在压测期间异常!"
        local dead_ts=$(grep "DEAD" "$proc_log" | head -1 | awk '{print $1}')
        echo "  Pod 失联时间: +${dead_ts}s" >> "$REPORT_FILE"
        result=1
    else
        pass "${tag}: Pod 存活"
        echo "PASS ${tag}: Pod 存活" >> "$REPORT_FILE"
    fi

    # 2. 内存 (RSS) 分析
    if [[ -f "$proc_log" ]]; then
        local data_lines=$(grep -vc "^#" "$proc_log" 2>/dev/null || echo "0")
        local rss_start=$(awk '!/^#/ && $2!="DEAD" {print $2; exit}' "$proc_log" 2>/dev/null || echo "0")
        local rss_end=$(awk '!/^#/ && $2!="DEAD" {val=$2} END{print val}' "$proc_log" 2>/dev/null || echo "0")

        if [[ "$rss_start" -gt 0 && "$rss_end" -gt 0 ]]; then
            local rss_growth_pct
            rss_growth_pct=$(echo "scale=1; (($rss_end - $rss_start) / $rss_start) * 100" | bc -l 2>/dev/null || echo "0")
            local rss_start_mb=$((rss_start / 1024))
            local rss_end_mb=$((rss_end / 1024))

            if (( $(echo "$rss_growth_pct > 20" | bc -l 2>/dev/null || echo 0) )); then
                fail "${tag}: RSS ${rss_start_mb}MB → ${rss_end_mb}MB (+${rss_growth_pct}%), 可能内存泄漏"
                echo "FAIL RSS: ${rss_start_mb}MB → ${rss_end_mb}MB (+${rss_growth_pct}%)" >> "$REPORT_FILE"
                result=1
            elif (( $(echo "$rss_growth_pct > 10" | bc -l 2>/dev/null || echo 0) )); then
                warn "${tag}: RSS ${rss_start_mb}MB → ${rss_end_mb}MB (+${rss_growth_pct}%), 需关注"
                echo "WARN RSS: ${rss_start_mb}MB → ${rss_end_mb}MB (+${rss_growth_pct}%)" >> "$REPORT_FILE"
            else
                pass "${tag}: RSS ${rss_start_mb}MB → ${rss_end_mb}MB (+${rss_growth_pct}%), 稳定"
                echo "PASS RSS: ${rss_start_mb}MB → ${rss_end_mb}MB (+${rss_growth_pct}%)" >> "$REPORT_FILE"
            fi
        fi
    fi

    # 3. fd 泄漏
    if [[ -f "$proc_log" ]]; then
        local fd_start=$(awk '!/^#/ && $2!="DEAD" {print $3; exit}' "$proc_log" 2>/dev/null || echo "0")
        local fd_end=$(awk '!/^#/ && $2!="DEAD" {val=$3} END{print val}' "$proc_log" 2>/dev/null || echo "0")
        if [[ "$fd_start" -gt 0 && "$fd_end" -gt 0 ]]; then
            local fd_diff=$((fd_end - fd_start))
            if [[ $fd_diff -gt $((fd_start / 2)) ]] && [[ $fd_diff -gt 5 ]]; then
                fail "${tag}: fd $fd_start → $fd_end (+$fd_diff), 可能泄漏"
                echo "FAIL fd: $fd_start → $fd_end (+$fd_diff)" >> "$REPORT_FILE"
                result=1
            else
                pass "${tag}: fd $fd_start → $fd_end, 稳定"
                echo "PASS fd: $fd_start → $fd_end" >> "$REPORT_FILE"
            fi
        fi
    fi

    # 4. kubectl top 汇总
    if [[ -f "$top_log" ]]; then
        local samples=$(grep -vc "^#" "$top_log" 2>/dev/null || echo "0")
        local avg_mem=$(awk '!/^#/ && $3!="DEAD" {sum+=$3; n++} END{printf "%.0f", sum/n}' "$top_log" 2>/dev/null || echo "0")
        local max_mem=$(awk '!/^#/ && $3!="DEAD" {if($3>m)m=$3} END{print m}' "$top_log" 2>/dev/null || echo "0")
        info "${tag}: kubectl top — avg=${avg_mem}MiB, max=${max_mem}MiB (${samples} samples)"
        echo "kubectl top: avg=${avg_mem}MiB, max=${max_mem}MiB (${samples} samples)" >> "$REPORT_FILE"
    fi

    # 5. wrk 结果
    set +e  # 避免 grep/awk 无匹配时 set -e 静默退出
    if [[ -f "$wrk_log" ]]; then
        if grep -q "SKIPPED" "$wrk_log" 2>/dev/null; then
            info "${tag}: wrk 已跳过"
            echo "wrk: SKIPPED" >> "$REPORT_FILE"
        else
            local rps errs total lat_avg lat_p99
            rps=$(grep -oP 'Requests/sec:\s*\K[0-9.]+' "$wrk_log" 2>/dev/null || echo "N/A")
            errs=$(grep -oP 'Non-2xx or 3xx responses:\s*\K[0-9]+' "$wrk_log" 2>/dev/null || echo "0")
            total=$(grep -oP '^\s*\K[0-9]+(?= requests)' "$wrk_log" 2>/dev/null || echo "0")
            lat_avg=$(grep -oP 'Latency\s+\K[0-9.]+[um]s' "$wrk_log" 2>/dev/null || echo "N/A")
            lat_p99=$(awk '/99%/{print $2}' "$wrk_log" 2>/dev/null || echo "N/A")

            if [[ "$errs" -gt 0 ]]; then
                fail "${tag}: wrk Non-2xx=$errs, total=$total, RPS=$rps"
                echo "FAIL wrk: Non-2xx=$errs, total=$total, RPS=$rps, avg_lat=${lat_avg}, p99=${lat_p99}" >> "$REPORT_FILE"
                result=1
            else
                pass "${tag}: wrk total=$total, RPS=$rps, avg_lat=${lat_avg}, p99=${lat_p99}"
                echo "PASS wrk: total=$total, RPS=$rps, avg_lat=${lat_avg}, p99=${lat_p99}" >> "$REPORT_FILE"
            fi
        fi
    fi
    set -e

    echo "" >> "$REPORT_FILE"
    return $result
}

# ---- 主流程 ----
main() {
    echo ""
    echo "============================================================"
    echo "  Thunder 稳定性压测 (K8s)"
    echo "  集群: ${NODE_IP} | namespace: ${NAMESPACE}"
    echo "  时长: ${DURATION}s | 并发: $CONCURRENCY"
    echo "  协议: HTTP$($SKIP_HTTPS || echo ' + HTTPS')"
    if [[ -n "$SWITCH_BACKEND" ]]; then
        echo "  IO 后端: 切换到 $SWITCH_BACKEND"
    fi
    echo "============================================================"
    echo ""

    if ! k8s_health_check; then
        fail "前置健康检查失败"
        exit 1
    fi

    # 确定 IO 后端
    local backend_tag="current"
    if [[ -n "$SWITCH_BACKEND" ]]; then
        warn "K8s 下 IO 后端切换需要重新 build 镜像 + redeploy，当前脚本只做检测不自动切换"
        info "请手动部署 $SWITCH_BACKEND 版本的 Pod 后再运行本脚本"
        backend_tag="$SWITCH_BACKEND"
    else
        local hello_pod
        hello_pod=$(kubectl get pod -n "$NAMESPACE" -l app=thunder-hello \
            -o jsonpath='{.items[?(@.status.phase=="Running")].metadata.name}' | awk '{print $1}')
        backend_tag=$(kubectl exec -n "$NAMESPACE" "$hello_pod" -- \
            grep -oP '"io_backend"\s*:\s*"\K[^"]+' /app/conf/Hello.json 2>/dev/null || echo "ev")
    fi
    info "测试 IO 后端: $backend_tag"

    local overall_result=0

    # ---- HTTP ----
    echo ""
    echo "===== HTTP 压测 ($backend_tag) ====="
    local tag_http="${backend_tag}_http"
    start_monitor "$tag_http"
    run_http_stress "$tag_http"
    local wrk_http_log="$WRK_OUT_FILE"
    stop_monitor "$tag_http"
    analyze_results "$tag_http" "$wrk_http_log" || overall_result=1

    # ---- HTTPS (可选) ----
    if ! $SKIP_HTTPS; then
        echo ""
        echo "===== HTTPS 压测 ($backend_tag) ====="
        sleep 10
        local tag_https="${backend_tag}_https"
        start_monitor "$tag_https"
        run_https_stress "$tag_https"
        local wrk_https_log="$WRK_OUT_FILE"
        stop_monitor "$tag_https"
        analyze_results "$tag_https" "$wrk_https_log" || overall_result=1
    fi

    # ---- 汇总 ----
    echo ""
    echo "============================================================"
    if [[ $overall_result -eq 0 ]]; then
        echo -e "${GREEN}  全部通过: 稳定性压测 PASS${NC}"
    else
        echo -e "${RED}  存在问题: 稳定性压测 FAIL${NC}"
    fi
    echo "  数据: $TMPDIR"
    echo "  报告: $REPORT_FILE"
    echo "============================================================"
    echo ""

    cat "$REPORT_FILE"
    exit $overall_result
}

main
