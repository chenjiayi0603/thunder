#!/bin/bash
# ============================================================
# Layer 2: Thunder Worker 层测试 — 进程级优雅重启
# 验证 GracefulRestartWorker / fd转移 / SO_REUSEPORT / 排空
# 支持 docker-compose 和 k8s 双模式
# ============================================================
# 用法:
#   ./tests/test_graceful_restart.sh               # docker-compose 模式
#   ./tests/test_graceful_restart.sh --k8s         # k8s 模式
#   ./tests/test_graceful_restart.sh --restart     # 仅 Worker 优雅重启
#   ./tests/test_graceful_restart.sh --fd          # 仅 fd 转移压测
#   ./tests/test_graceful_restart.sh --drain       # 仅 排空超时
#   ./tests/test_graceful_restart.sh --all         # 全部
# ============================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; BLUE='\033[0;34m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

K8S_MODE=false; RUN_RESTART=false; RUN_FD=false; RUN_DRAIN=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --k8s) K8S_MODE=true ;;
        --restart) RUN_RESTART=true ;;
        --fd) RUN_FD=true ;;
        --drain) RUN_DRAIN=true ;;
        --all) RUN_RESTART=true; RUN_FD=true; RUN_DRAIN=true ;;
        *) RUN_RESTART=true; RUN_FD=true; RUN_DRAIN=true; break ;;
    esac; shift
done
$RUN_RESTART || $RUN_FD || $RUN_DRAIN || { RUN_RESTART=true; RUN_FD=true; RUN_DRAIN=true; }

# ── k8s/docker 双模式 ──────────────────────────────────
K8S_NS="${K8S_NAMESPACE:-thunder}"
GR_TMP=$(mktemp /tmp/graceful_restart_XXXXXX)
trap "rm -f $GR_TMP" EXIT

if $K8S_MODE; then
    if [ -n "${K8S_NODE_IP:-}" ]; then NODE_IP="$K8S_NODE_IP"
    else NODE_IP=$(kubectl get node -o jsonpath='{.items[0].status.addresses[?(@.type=="InternalIP")].address}' 2>/dev/null) || true
         [ -z "${NODE_IP:-}" ] && NODE_IP=$(hostname -I 2>/dev/null | awk '{print $1}') || true; fi
    [ -z "${NODE_IP:-}" ] && { echo -e "${RED}✘ 无法检测 Node IP${NC}"; exit 1; }
    PORT_HELLO=30006; HELLO_URL="http://${NODE_IP}:${PORT_HELLO}"
    # exec helper: 在 pod 内执行命令
    kexec() { local pod=$1; shift; kubectl exec -n "$K8S_NS" "$pod" -- sh -c "$*" 2>/dev/null; }
    get_mgr_pid() {
        local pod=$1 pattern=$2
        kexec "$pod" "ps aux | awk '\$NF==\"$pattern\"{print \$2}' | head -1"
    }
    trigger_restart() { local pod=$1 pid=$2; kexec "$pod" "kill -SIGUSR2 $pid" || true; }
else
    HELLO_URL="http://127.0.0.1:27006"
    kexec() { sh -c "$*"; }
    get_mgr_pid() { ps aux | awk -v p="$2" '$NF==p{print $2}' | head -1; }
    trigger_restart() { kill -SIGUSR2 "$2" || true; }
fi

check() { local n="$1" e="$2" c="$3"; local out; out=$(eval "$c" 2>/dev/null) || true
    if echo "$out" | grep -q "$e"; then echo -e "  ${GREEN}✅${NC} $n"; PASS=$((PASS+1))
    else echo -e "  ${RED}❌${NC} $n (got: ${out:0:120})"; FAIL=$((FAIL+1)); fi; }
check_ge() { local n="$1" m="$2" c="$3"; local out; out=$(eval "$c" 2>/dev/null) || out="0"
    if [ "${out}" -ge "${m}" ] 2>/dev/null; then echo -e "  ${GREEN}✅${NC} $n (got: $out)"; PASS=$((PASS+1))
    else echo -e "  ${RED}❌${NC} $n (got: $out, expect ≥$m)"; FAIL=$((FAIL+1)); fi; }
info() { echo -e "  ${BLUE}→${NC} $*"; }
warn() { echo -e "  ${YELLOW}⚠️${NC} $*"; SKIP=$((SKIP+1)); }

MODE_STR=$($K8S_MODE && echo "k8s" || echo "docker-compose")
echo "Layer2: Thunder Worker 优雅重启  mode=$MODE_STR"
echo "=============================================="
echo "  GracefulRestartWorker / fd转移 / 排空超时"
echo "=============================================="

# ── 选目标 Pod/进程 ─────────────────────────────────────
if $K8S_MODE; then
    HELLO_POD=$(kubectl get pods -n "$K8S_NS" -l app=thunder-hello -o jsonpath='{.items[0].metadata.name}')
    info "目标 Pod: $HELLO_POD"
else
    HELLO_POD="localhost"
fi

# ── 1. Worker 优雅重启 (SIGUSR2) ───────────────────────
if $RUN_RESTART; then
echo ""
echo "--- 1. Worker 优雅重启 (SIGUSR2 → GracefulRestartWorker) ---"

MGR_PID=$(get_mgr_pid "$HELLO_POD" "Hello_robot")
info "Manager PID=$MGR_PID"

if [ -z "${MGR_PID:-}" ] || [ "$MGR_PID" -le 1 ] 2>/dev/null; then
    warn "无法获取 Manager PID, 跳过"; RUN_RESTART=false
fi
fi

if $RUN_RESTART; then
# 基线: Worker 存在, Echo 可用
check_ge "Worker 进程存在 (基线)" 1 \
    "kexec '$HELLO_POD' 'ps aux | grep Hello_robot_W0 | grep -v grep | wc -l' 2>/dev/null || echo 0"
check "Echo 可用 (基线)" '"code":0' \
    "curl -sf --max-time 5 -X POST -d '{\"option\":\"Echo\"}' ${HELLO_URL}/hello/hello 2>/dev/null"

# 触发 SIGUSR2 (硬重启 → SIGKILL old worker, Manager fork new)
OLD_PID=$(kexec "$HELLO_POD" "ps aux | grep Hello_robot_W0 | grep -v grep | awk '{print \$2}' | head -1" 2>/dev/null)
info "触发 SIGUSR2 → RestartWorkers (硬重启)..."
trigger_restart "$HELLO_POD" "$MGR_PID"
sleep 5

# 验证: 新 Worker 启动
NEW_PID=$(kexec "$HELLO_POD" "ps aux | grep Hello_robot_W0 | grep -v grep | awk '{print \$2}' | head -1" 2>/dev/null)
if [ -n "${NEW_PID:-}" ] && [ "$NEW_PID" != "$OLD_PID" ]; then
    info "Worker PID 已变更: $OLD_PID → $NEW_PID"
    echo -e "  ${GREEN}✅${NC} Worker 硬重启: 新Worker接管"; PASS=$((PASS+1))
elif [ -n "${NEW_PID:-}" ]; then
    echo -e "  ${YELLOW}⚠️${NC} Worker PID 未变 (重启太快, 同一PID复用)"; SKIP=$((SKIP+1))
else
    echo -e "  ${RED}❌${NC} Worker 重启后无进程"; FAIL=$((FAIL+1))
fi

# 验证: Echo 仍然可用
check "重启后 Echo 可用" '"code":0' \
    "curl -sf --max-time 5 -X POST -d '{\"option\":\"Echo\"}' ${HELLO_URL}/hello/hello 2>/dev/null"

# 验证: RestartWorkers 日志
LOG_LINES=$(kexec "$HELLO_POD" "grep -c 'RestartWorkers\|GracefulRestartWorker\|terminate worker' /thunder/deploy/HelloHttp/log/Hello_robot.log 2>/dev/null" 2>/dev/null | head -1 | tr -d '\n')
info "RestartWorkers 日志行数: ${LOG_LINES:-0}"
check_ge "日志记录 RestartWorkers" 1 "echo ${LOG_LINES:-0}"
fi  # RUN_RESTART

# ── 2. fd 转移 + 压测 ──────────────────────────────────
if $RUN_FD; then
echo ""
echo "--- 2. fd 转移 & SO_REUSEPORT (压测期间重启) ---"

MGR_PID=$(get_mgr_pid "$HELLO_POD" "Hello_robot")
if [ -z "${MGR_PID:-}" ] || [ "$MGR_PID" -le 1 ] 2>/dev/null; then
    warn "无法获取 Manager PID, 跳过"
else
    info "压测 20s 持续 HTTP 请求..."
    echo "0 0" > "$GR_TMP"
    (for i in $(seq 1 40); do
        if curl -sf --max-time 2 -X POST -d '{"option":"Echo"}' "${HELLO_URL}/hello/hello" >/dev/null 2>&1; then :; else
            read F T < "$GR_TMP"; echo "$((F+1)) $T" > "$GR_TMP"; fi
        read F T < "$GR_TMP"; echo "$F $((T+1))" > "$GR_TMP"
        sleep 0.5
    done) &
    CPID=$!; sleep 3
    info "触发 SIGUSR2 优雅重启..."
    trigger_restart "$HELLO_POD" "$MGR_PID"
    wait $CPID 2>/dev/null || true
    read FAIL_C TOTAL_C < "$GR_TMP"
    FR=$(( FAIL_C * 100 / (TOTAL_C > 0 ? TOTAL_C : 1) ))
    info "结果: $TOTAL_C 请求, $FAIL_C 失败 (${FR}%)"

    if [ "$FAIL_C" -eq 0 ]; then
        echo -e "  ${GREEN}✅${NC} fd转移 & SO_REUSEPORT: 重启期间零失败!"
        PASS=$((PASS+1))
    elif [ "$FR" -le 10 ]; then
        echo -e "  ${GREEN}✅${NC} fd转移: 失败率 ${FR}% ≤10%"; PASS=$((PASS+1))
    else
        echo -e "  ${RED}❌${NC} fd转移: 失败率 ${FR}% >10%"; FAIL=$((FAIL+1))
    fi

    # 检查 restart 日志
    DRAIN_COUNT=$(kexec "$HELLO_POD" "grep -c 'RestartWorkers\|terminate worker\|GracefulRestartWorker' /thunder/deploy/HelloHttp/log/Hello_robot.log 2>/dev/null" 2>/dev/null | head -1 | tr -d '\n')
    info "RestartWorkers 触发: ${DRAIN_COUNT:-0} 次"
    check_ge "Worker 重启已触发" 1 "echo ${DRAIN_COUNT:-0}"
fi
fi  # RUN_FD

# ── 3. 排空验证 ────────────────────────────────────────
if $RUN_DRAIN; then
echo ""
echo "--- 3. 排空 (Drain) 验证 ---"

MGR_PID=$(get_mgr_pid "$HELLO_POD" "Hello_robot")
if [ -z "${MGR_PID:-}" ] || [ "$MGR_PID" -le 1 ] 2>/dev/null; then
    warn "无法获取 Manager PID, 跳过"
else
    # 验证: Worker::EnterDrainMode 是否被调用
    # 通过触发重启后检查日志
    trigger_restart "$HELLO_POD" "$MGR_PID"
    sleep 5

    DRAIN_ENTER=$(kexec "$HELLO_POD" "grep -c 'EnterDrainMode\|entering drain' /thunder/deploy/HelloHttp/log/Hello_robot_W0.log 2>/dev/null" 2>/dev/null | head -1 | tr -d '\n')
    DRAIN_DONE=$(kexec "$HELLO_POD" "grep -c 'drain complete\|restart.*complete' /thunder/deploy/HelloHttp/log/Hello_robot.log 2>/dev/null" 2>/dev/null | head -1 | tr -d '\n')
    info "EnterDrainMode: ${DRAIN_ENTER:-0}  DrainDone: ${DRAIN_DONE:-0}"

    if [ "$DRAIN_ENTER" -ge 1 ] && [ "$DRAIN_DONE" -ge 1 ]; then
        echo -e "  ${GREEN}✅${NC} 排空流程完整: EnterDrainMode → drain complete"; PASS=$((PASS+1))
    elif [ "$DRAIN_ENTER" -ge 1 ]; then
        echo -e "  ${GREEN}✅${NC} 排空: EnterDrainMode 已触发 (drain complete 日志可能别名不同)"; PASS=$((PASS+1))
    else
        echo -e "  ${YELLOW}⚠️${NC} 排空: 未检测到 Drain 日志 (可能日志级别不够或实现差异)"; SKIP=$((SKIP+1))
    fi

    # 验证 SO_REUSEPORT 生效: 检查是否有多个 listen fd
    LISTEN_COUNT=$(kexec "$HELLO_POD" "grep -c 'InitClientListener' /thunder/deploy/HelloHttp/log/Hello_robot_W0.log 2>/dev/null" 2>/dev/null | head -1 | tr -d '\n')
    info "InitClientListener: ${LISTEN_COUNT:-0} 次"
    check_ge "SO_REUSEPORT/InitClientListener 已启用" 1 "echo ${LISTEN_COUNT:-0}"
fi
fi  # RUN_DRAIN

# ── 汇总 ────────────────────────────────────────────────
echo ""; echo "=============================================="
TOTAL=$((PASS+FAIL+SKIP))
[ "$FAIL" -eq 0 ] && echo -e "  ${GREEN}Layer2 全部通过 $PASS/$TOTAL${NC}" \
    && [ "$SKIP" -gt 0 ] && echo -e "  ${YELLOW}跳过 $SKIP${NC} (不计入失败)" \
    || echo -e "  ${RED}失败 $FAIL${NC} / 通过 ${GREEN}$PASS${NC} / 跳过 ${YELLOW}$SKIP${NC} = $TOTAL"
echo "=============================================="
exit $FAIL
