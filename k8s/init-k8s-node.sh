#!/usr/bin/env bash
# =============================================================================
# Thunder K8s Node 性能初始化脚本 (#154)
# =============================================================================
# 在新 K8s worker/control-plane 上跑一次，自动配置所有 Node 层性能优化。
#
# 功能:
#   1. kubelet CPU Manager static policy       → 为 Guaranteed QoS Pod 绑核
#   2. kubelet Topology Manager (多 NUMA 时)   → 为 Guaranteed QoS Pod 绑 NUMA
#   3. kubelet allowedUnsafeSysctls            → 允许 Pod 设置 tcp_rmem/wmem
#   4. CPU governor → performance             → 消除 powersave 导致的性能损失
#   5. Transparent Hugepage → madvise         → 按需使用大页
#   6. Node 级 sysctl (TCP buffer)             → Pod 级 sysctl 的双层保护
#
# 运行:
#   sudo bash k8s/init-k8s-node.sh
#   sudo bash k8s/init-k8s-node.sh --dry-run    # 只检查不改动
#
# 要求:
#   - Root 权限 (sudo)
#   - kubeadm 部署的 K8s 集群
#   - 内核 >= 4.15
# =============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()    { echo -e "${BLUE}[INFO]${NC}  $*"; }
success() { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*"; }

DRY_RUN=false
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=true && info "干运行模式: 只检查不改动"

KUBELET_CONFIG="/var/lib/kubelet/config.yaml"
KUBELET_EXTRA_ARGS_FILE="/var/lib/kubelet/kubeadm-flags.env"
KUBELET_DEFAULT_ARGS="/etc/default/kubelet"
NEED_KUBELET_RESTART=false
CHANGES_MADE=0

# =============================================================================
# 安全检查
# =============================================================================
check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "此脚本需要 root 权限: sudo bash k8s/init-k8s-node.sh"
        exit 1
    fi
}

check_kubelet_config() {
    if [[ ! -f "$KUBELET_CONFIG" ]]; then
        error "找不到 kubelet 配置文件: $KUBELET_CONFIG"
        error "此脚本仅支持 kubeadm 部署的标准 K8s 集群"
        exit 1
    fi
}

# =============================================================================
# 1. CPU Manager — Static Policy (绑核)
# =============================================================================
configure_cpu_manager() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  1. CPU Manager — Static Policy (CPU 绑核)"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local current
    current=$(grep -E '^\s*cpuManagerPolicy:' "$KUBELET_CONFIG" 2>/dev/null | awk '{print $2}' || echo "none")

    info "当前 cpuManagerPolicy: ${current:-none}"

    if [[ "$current" == "static" ]]; then
        success "cpuManagerPolicy 已是 static"
        return 0
    fi

    if $DRY_RUN; then
        warn "[DRY-RUN] 将设置 cpuManagerPolicy: static"
        return 0
    fi

    local backup="${KUBELET_CONFIG}.bak.$(date +%s)"
    cp "$KUBELET_CONFIG" "$backup"
    info "已备份 → $backup"

    if grep -q 'cpuManagerPolicy:' "$KUBELET_CONFIG" 2>/dev/null; then
        sed -i 's/^.*cpuManagerPolicy:.*/cpuManagerPolicy: static/' "$KUBELET_CONFIG"
    else
        echo "cpuManagerPolicy: static" >> "$KUBELET_CONFIG"
    fi

    # 防止系统进程饥饿
    if ! grep -q '^kubeReserved:' "$KUBELET_CONFIG" 2>/dev/null; then
        cat >> "$KUBELET_CONFIG" <<'YAML'
kubeReserved:
  cpu: "500m"
  memory: "512Mi"
YAML
        info "已添加 kubeReserved: cpu=500m, memory=512Mi"
    fi

    # ⚠️ 删除旧 checkpoint: policy none→static 切换时 kubelet 拒绝启动
    if [[ -f /var/lib/kubelet/cpu_manager_state ]]; then
        rm -f /var/lib/kubelet/cpu_manager_state
        info "已删除旧 cpu_manager_state checkpoint (none → static 切换必须)"
    fi

    success "已设置 cpuManagerPolicy: static"
    NEED_KUBELET_RESTART=true
    CHANGES_MADE=$((CHANGES_MADE + 1))
}

# =============================================================================
# 2. Topology Manager — Single-NUMA-Node (绑 NUMA)
# =============================================================================
configure_topology_manager() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  2. Topology Manager — NUMA 亲和性"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local numa_nodes
    numa_nodes=$(numactl --hardware 2>/dev/null | grep "^available:" | awk '{print $2}' || echo "1")
    info "NUMA 节点数: $numa_nodes"

    if [[ "$numa_nodes" -le 1 ]]; then
        info "单 NUMA 节点: 绑 NUMA 无意义，跳过"
        return 0
    fi

    local current
    current=$(grep -E '^\s*topologyManagerPolicy:' "$KUBELET_CONFIG" 2>/dev/null | awk '{print $2}' || echo "none")
    info "当前 topologyManagerPolicy: ${current:-none}"

    if [[ "$current" == "single-numa-node" ]]; then
        success "topologyManagerPolicy 已是 single-numa-node"
        return 0
    fi

    if $DRY_RUN; then
        warn "[DRY-RUN] 将设置 topologyManagerPolicy: single-numa-node"
        return 0
    fi

    if grep -q 'topologyManagerPolicy:' "$KUBELET_CONFIG" 2>/dev/null; then
        sed -i 's/^.*topologyManagerPolicy:.*/topologyManagerPolicy: single-numa-node/' "$KUBELET_CONFIG"
    else
        echo "topologyManagerPolicy: single-numa-node" >> "$KUBELET_CONFIG"
    fi

    success "已设置 topologyManagerPolicy: single-numa-node"
    NEED_KUBELET_RESTART=true
    CHANGES_MADE=$((CHANGES_MADE + 1))
}

# =============================================================================
# 3. kubelet allowedUnsafeSysctls — 允许 Pod 设置 TCP buffer
# =============================================================================
configure_kubelet_sysctls() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  3. kubelet allowedUnsafeSysctls — TCP buffer Pod 级配置"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local required="net.ipv4.tcp_rmem,net.ipv4.tcp_wmem"

    # 检查 kubelet 启动参数
    local current
    current=$(ps aux | grep '[k]ubelet' | grep -oP 'allowed-unsafe-sysctls=\K[^ ]*' 2>/dev/null || echo "")

    if [[ "$current" == *"tcp_rmem"* ]] && [[ "$current" == *"tcp_wmem"* ]]; then
        success "allowedUnsafeSysctls 已包含 tcp_rmem, tcp_wmem"
        return 0
    fi

    # 构建新值
    local new_value
    if [[ -z "$current" ]]; then
        new_value="$required"
    else
        new_value="${current},${required}"
    fi

    if $DRY_RUN; then
        warn "[DRY-RUN] 将添加 allowedUnsafeSysctls: $required"
        return 0
    fi

    # 选择正确的配置写入位置
    local flags_file=""
    if [[ -f "$KUBELET_EXTRA_ARGS_FILE" ]]; then
        flags_file="$KUBELET_EXTRA_ARGS_FILE"
    elif [[ -f "$KUBELET_DEFAULT_ARGS" ]]; then
        flags_file="$KUBELET_DEFAULT_ARGS"
    fi

    if [[ -n "$flags_file" ]]; then
        local backup="${flags_file}.bak.$(date +%s)"
        cp "$flags_file" "$backup"
        info "已备份 → $backup"

        if grep -q 'allowed-unsafe-sysctls' "$flags_file" 2>/dev/null; then
            sed -i "s/allowed-unsafe-sysctls=\([^\" ]*\)/allowed-unsafe-sysctls=\1,${required}/" "$flags_file"
        else
            if grep -q 'KUBELET_EXTRA_ARGS=' "$flags_file" 2>/dev/null; then
                sed -i "s/KUBELET_EXTRA_ARGS=\"\(.*\)\"/KUBELET_EXTRA_ARGS=\"\1 --allowed-unsafe-sysctls=${required}\"/" "$flags_file"
            else
                echo "KUBELET_EXTRA_ARGS=\"--allowed-unsafe-sysctls=${required}\"" >> "$flags_file"
            fi
        fi
        success "已添加 allowedUnsafeSysctls: $required"
        NEED_KUBELET_RESTART=true
        CHANGES_MADE=$((CHANGES_MADE + 1))
    else
        warn "未找到 kubelet extra args 文件"
        warn "Pod 级 tcp_rmem/tcp_wmem 将不可用 → Node 级 sysctl 作为回退"
    fi
}

# =============================================================================
# 4. CPU Governor → performance
# =============================================================================
configure_cpu_governor() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  4. CPU Governor → performance"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local gov_changed=0
    local total=0

    for gov_file in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        total=$((total + 1))
        local cur
        cur=$(cat "$gov_file" 2>/dev/null || echo "N/A")
        if [[ "$cur" == "performance" ]]; then continue; fi

        if $DRY_RUN; then
            [[ $gov_changed -eq 0 ]] && warn "[DRY-RUN] 检测到非 performance: $cur"
            gov_changed=1; continue
        fi

        local cpu_dir="${gov_file%/scaling_governor}"
        if grep -q "performance" "${cpu_dir}/scaling_available_governors" 2>/dev/null; then
            echo "performance" > "$gov_file" 2>/dev/null || true
            gov_changed=1
        fi
    done

    if [[ $gov_changed -eq 0 ]]; then
        success "所有 $total 个 CPU 已是 performance"
    elif ! $DRY_RUN; then
        success "已将 $total 个 CPU governor 设为 performance"
        CHANGES_MADE=$((CHANGES_MADE + 1))
    fi

    # 持久化: systemd service
    if ! $DRY_RUN && [[ $gov_changed -gt 0 ]]; then
        command -v cpupower &>/dev/null && cpupower frequency-set -g performance &>/dev/null || true

        local svc="/etc/systemd/system/cpu-governor-performance.service"
        if [[ ! -f "$svc" ]]; then
            cat > "$svc" <<'UNIT'
[Unit]
Description=Set CPU governor to performance
After=multi-user.target
[Service]
Type=oneshot
ExecStart=/bin/bash -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$f" 2>/dev/null; done'
RemainAfterExit=yes
[Install]
WantedBy=multi-user.target
UNIT
            systemctl daemon-reload
            systemctl enable cpu-governor-performance.service
            success "已创建持久化 service: cpu-governor-performance"
        fi
    fi
}

# =============================================================================
# 5. Transparent Hugepage → madvise
# =============================================================================
configure_hugepage() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  5. Transparent Hugepage → madvise"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local enabled defrag
    enabled=$(grep -oP '\[\K[^]]+' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "?")
    defrag=$(grep -oP '\[\K[^]]+' /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || echo "?")

    info "THP: enabled=$enabled, defrag=$defrag"

    if [[ "$enabled" == "madvise" ]] && [[ "$defrag" == "madvise" ]]; then
        success "THP 已是 madvise"
        return 0
    fi

    if $DRY_RUN; then
        warn "[DRY-RUN] 将设 THP: madvise/madvise"
        return 0
    fi

    echo "madvise" > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
    echo "madvise" > /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || true

    local svc="/etc/systemd/system/thp-madvise.service"
    if [[ ! -f "$svc" ]]; then
        cat > "$svc" <<'UNIT'
[Unit]
Description=Set THP to madvise
After=multi-user.target
[Service]
Type=oneshot
ExecStart=/bin/bash -c 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled && echo madvise > /sys/kernel/mm/transparent_hugepage/defrag'
RemainAfterExit=yes
[Install]
WantedBy=multi-user.target
UNIT
        systemctl daemon-reload
        systemctl enable thp-madvise.service
        success "已创建持久化 service: thp-madvise"
    fi

    success "已设 THP: enabled=madvise, defrag=madvise"
    CHANGES_MADE=$((CHANGES_MADE + 1))
}

# =============================================================================
# 6. Node 级 sysctl — TCP/网络优化 (Pod 级回退)
# =============================================================================
configure_node_sysctls() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  6. Node 级 sysctl — TCP/网络优化"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local sysctl_file="/etc/sysctl.d/99-thunder-performance.conf"

    if $DRY_RUN; then
        warn "[DRY-RUN] 将创建 $sysctl_file (tcp_rmem/wmem/somaxconn/backlog/fastopen)"
        return 0
    fi

    cat > "$sysctl_file" <<'EOF'
# Thunder K8s 性能优化 sysctl (#154)
# 由 k8s/init-k8s-node.sh 自动生成

# TCP Buffer: 低延迟优先，最大化并发连接
net.ipv4.tcp_rmem = 4096 16384 32768
net.ipv4.tcp_wmem = 4096 16384 32768

# Accept/Backlog: 高并发连接爆发保护
net.core.somaxconn = 32768
net.ipv4.tcp_max_syn_backlog = 8192
net.core.netdev_max_backlog = 5000

# TCP Fast Open: 减少 1 RTT
net.ipv4.tcp_fastopen = 3

# TIME_WAIT 复用
net.ipv4.tcp_tw_reuse = 1

# 长连接: 禁用空闲后慢启动
net.ipv4.tcp_slow_start_after_idle = 0

# KeepAlive: 更快检测死连接
net.ipv4.tcp_keepalive_time = 60
net.ipv4.tcp_keepalive_intvl = 10
net.ipv4.tcp_keepalive_probes = 6
EOF

    sysctl -p "$sysctl_file" &>/dev/null || true
    success "已创建并应用 $sysctl_file"
    CHANGES_MADE=$((CHANGES_MADE + 1))
}

# =============================================================================
# 重启 kubelet
# =============================================================================
restart_kubelet_if_needed() {
    if ! $NEED_KUBELET_RESTART; then
        echo ""; info "kubelet 配置未变更，无需重启"; return 0
    fi

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  7. 重启 kubelet 使配置生效"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    if $DRY_RUN; then warn "[DRY-RUN] 将重启 kubelet"; return 0; fi

    info "重启 kubelet..."
    systemctl restart kubelet
    sleep 3

    if systemctl is-active --quiet kubelet; then
        success "kubelet 重启成功"
    else
        error "kubelet 启动失败! journalctl -u kubelet -n 50"
        return 1
    fi

    # 等待节点 Ready
    for i in $(seq 1 30); do
        local ns
        ns=$(kubectl get node "$(hostname)" -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}' 2>/dev/null || echo "")
        [[ "$ns" == "True" ]] && success "节点已 Ready" && return 0
        sleep 2
    done
    warn "节点 60s 未 Ready，请手动检查"
}

# =============================================================================
# 验证
# =============================================================================
verify() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  验证配置"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local all_ok=true check

    # CPU Manager
    check=$(grep -oP 'cpuManagerPolicy:\s*\K\S+' "$KUBELET_CONFIG" 2>/dev/null || echo "none")
    printf "  cpuManagerPolicy: "; [[ "$check" == "static" ]] && echo -e "${GREEN}$check ✅${NC}" || { echo -e "${RED}$check ❌${NC}"; all_ok=false; }

    # Topology Manager
    local nn=$(numactl --hardware 2>/dev/null | grep "^available:" | awk '{print $2}' || echo "1")
    if [[ $nn -ge 2 ]]; then
        check=$(grep -oP 'topologyManagerPolicy:\s*\K\S+' "$KUBELET_CONFIG" 2>/dev/null || echo "none")
        printf "  topologyManagerPolicy: "; [[ "$check" == "single-numa-node" ]] && echo -e "${GREEN}$check ✅${NC}" || { echo -e "${RED}$check ❌${NC}"; all_ok=false; }
    else
        echo "  topologyManagerPolicy: ⏭️ 跳过 (单 NUMA)"
    fi

    # CPU Governor
    check=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "N/A")
    printf "  CPU governor: "; [[ "$check" == "performance" ]] && echo -e "${GREEN}$check ✅${NC}" || { echo -e "${RED}$check ❌${NC}"; all_ok=false; }

    # THP
    local te td
    te=$(grep -oP '\[\K[^]]+' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "?")
    td=$(grep -oP '\[\K[^]]+' /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || echo "?")
    printf "  THP: "; [[ "$te" == "madvise" && "$td" == "madvise" ]] && echo -e "${GREEN}$te/$td ✅${NC}" || { echo -e "${RED}$te/$td ❌${NC}"; all_ok=false; }

    # sysctl
    check=$(sysctl -n net.ipv4.tcp_rmem 2>/dev/null | awk '{print $1}')
    printf "  sysctl tcp_rmem: "; [[ "$check" -le 4096 ]] && echo -e "${GREEN}$(sysctl -n net.ipv4.tcp_rmem) ✅${NC}" || { echo -e "${RED}$(sysctl -n net.ipv4.tcp_rmem) ❌${NC}"; all_ok=false; }

    # kubelet sysctls
    check=$(ps aux | grep '[k]ubelet' | grep -oP 'allowed-unsafe-sysctls=\K[^ ]*' 2>/dev/null || echo "(未设置)")
    printf "  allowedUnsafeSysctls: "
    if [[ "$check" == *"tcp_rmem"* && "$check" == *"tcp_wmem"* ]]; then
        echo -e "${GREEN}已配置 ✅${NC}"
    else
        echo -e "${YELLOW}$check ⚠️${NC}"; warn "  Pod 级 tcp_rmem/tcp_wmem 不可用，Node 级已回退"
    fi

    echo ""
    $all_ok && success "核心配置全部通过 ✅" || warn "部分配置未生效，检查 ❌ 标记项"
}

# =============================================================================
# 主流程
# =============================================================================
main() {
    echo ""
    echo "╔══════════════════════════════════════════════════════════════════╗"
    echo "║   Thunder K8s Node 性能初始化 (#154)                            ║"
    echo "║   CPU Manager | NUMA | sysctl | hugepage | governor             ║"
    echo "╚══════════════════════════════════════════════════════════════════╝"

    check_root
    check_kubelet_config

    configure_cpu_manager
    configure_topology_manager
    configure_kubelet_sysctls
    configure_cpu_governor
    configure_hugepage
    configure_node_sysctls
    restart_kubelet_if_needed

    verify

    echo ""
    echo "╔══════════════════════════════════════════════════════════════════╗"
    echo "║  完成 — $CHANGES_MADE 项变更                                              ║"
    echo "║  下一步:                                                         ║"
    echo "║  1. kubectl apply -f k8s/  (重新部署启用 Guaranteed QoS)        ║"
    echo "║  2. kubectl get pods -o wide -n thunder                         ║"
    echo "║  3. 验证 cgroup: kubectl exec <pod> -- cat /proc/self/status    ║"
    echo "╚══════════════════════════════════════════════════════════════════╝"
}

main "$@"
