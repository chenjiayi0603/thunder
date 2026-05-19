#!/usr/bin/env bash
# =============================================================================
# Thunder DPDK 环境初始化脚本
# =============================================================================
# 用法:
#   ./scripts/setup_dpdk.sh           # 初始化 hugepages + 验证 DPDK
#   ./scripts/setup_dpdk.sh status    # 仅显示状态
#   ./scripts/setup_dpdk.sh clean     # 清理 hugepages
#   ./scripts/setup_dpdk.sh test      # 编译并运行 DPDK 数据面测试
#
# 前置条件:
#   - sudo 权限（用于 hugepages 分配和 memlock 限制）
#   - DPDK 25.11 已提取到 ~/.local/dpdk/
# =============================================================================
set -euo pipefail

DPDK_ROOT="${HOME}/.local/dpdk"
DPDK_LIB="${DPDK_ROOT}/usr/lib/x86_64-linux-gnu"
DPDK_PMD="${DPDK_LIB}/dpdk/pmds-26.0"
NR_HUGEPAGES=256
SUDO_PASS="1q2w3e"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
ok()  { echo -e "${GREEN}[OK]${NC} $*"; }
warn(){ echo -e "${YELLOW}[WARN]${NC} $*"; }
fail(){ echo -e "${RED}[FAIL]${NC} $*"; }

check_sudo() {
    if ! echo "$SUDO_PASS" | sudo -S true 2>/dev/null; then
        fail "sudo 验证失败"
        exit 1
    fi
}

setup_hugepages() {
    local cur=$(cat /proc/meminfo | grep HugePages_Total | awk '{print $2}')
    if [ "$cur" -ge "$NR_HUGEPAGES" ]; then
        ok "hugepages: ${cur} (>= ${NR_HUGEPAGES})"
    else
        echo "  hugepages: ${cur} -> ${NR_HUGEPAGES}"
        echo "$SUDO_PASS" | sudo -S sysctl -w vm.nr_hugepages=$NR_HUGEPAGES >/dev/null 2>&1
        ok "hugepages 已设置"
    fi
    if ! mountpoint -q /mnt/huge 2>/dev/null; then
        echo "$SUDO_PASS" | sudo -S mkdir -p /mnt/huge /dev/hugepages 2>/dev/null
        mountpoint -q /dev/hugepages 2>/dev/null || \
            echo "$SUDO_PASS" | sudo -S mount -t hugetlbfs nodev /dev/hugepages 2>/dev/null || true
        echo "$SUDO_PASS" | sudo -S mount -t hugetlbfs nodev /mnt/huge 2>/dev/null
    fi
    echo "$SUDO_PASS" | sudo -S chmod 777 /dev/hugepages /mnt/huge 2>/dev/null || true
}

setup_memlock() {
    echo "$SUDO_PASS" | sudo -S prlimit --pid=$$ --memlock=unlimited 2>/dev/null || true
    ulimit -l unlimited 2>/dev/null || warn "memlock=$(ulimit -l)"
    ok "memlock: $(ulimit -l)"
}

check_dpdk() {
    if [ -f "${DPDK_LIB}/librte_eal.so" ]; then
        ok "DPDK 25.11: ${DPDK_ROOT}"
    else
        fail "DPDK lib not found: ${DPDK_LIB}"
        exit 1
    fi
}

export_env() {
    export LD_LIBRARY_PATH="${DPDK_LIB}:${LD_LIBRARY_PATH:-}"
    export PKG_CONFIG_PATH="${DPDK_LIB}/pkgconfig:${PKG_CONFIG_PATH:-}"
}

show_status() {
    echo ""
    echo "=========================================="
    echo "  Thunder DPDK 环境状态"
    echo "=========================================="
    echo "  HugePages: $(cat /proc/meminfo | grep HugePages_Total | awk '{print $2}')"
    echo "  HugeFree:  $(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')"
    echo "  Memlock:   $(ulimit -l)"
    echo "  DPDK:      ${DPDK_ROOT}"
    echo "  PMD ring:  $(test -f ${DPDK_PMD}/librte_net_ring.so && echo OK || echo MISSING)"
    echo "  PMD mempool_ring: $(test -f ${DPDK_PMD}/librte_mempool_ring.so && echo OK || echo MISSING)"
    echo ""
}

run_test() {
    export_env
    echo "=== Thunder DPDK 数据面测试 ==="
    
    cat > /tmp/thunder_dpdk_test.c << 'CEOF'
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_version.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    rte_eal_init(argc, argv);
    uint16_t nb = rte_eth_dev_count_avail();
    printf("DPDK %s | ports=%u | hp=%d\n", rte_version(), nb, rte_eal_has_hugepages());
    if (nb < 2) { rte_eal_cleanup(); return 1; }
    struct rte_mempool *mp = rte_pktmbuf_pool_create("p", 4095, 0, 0, 2176, 0);
    printf("Pool: %s\n", mp?"OK":rte_strerror(rte_errno));
    if (!mp) { rte_eal_cleanup(); return 1; }
    for (uint16_t pi = 0; pi < 2; pi++) {
        struct rte_eth_conf c = {0};
        rte_eth_dev_configure(pi, 1, 1, &c);
        rte_eth_rx_queue_setup(pi, 0, 128, 0, NULL, mp);
        rte_eth_tx_queue_setup(pi, 0, 128, 0, NULL);
        rte_eth_dev_start(pi);
    }
    const char *msg = "THUNDER_DPDK_OK";
    int len = strlen(msg) + 1;
    struct rte_mbuf *tx = rte_pktmbuf_alloc(mp);
    memcpy(rte_pktmbuf_append(tx, len), msg, len);
    rte_eth_tx_burst(1, 0, &tx, 1);
    struct rte_mbuf *rx[32];
    uint16_t nrx = 0;
    for (int i = 0; i < 10000 && nrx == 0; i++) nrx = rte_eth_rx_burst(0, 0, rx, 32);
    printf("RX: %u pkts\n", nrx);
    int ok = (nrx > 0 && memcmp(rte_pktmbuf_mtod(rx[0], char*), msg, len) == 0);
    if (nrx > 0) rte_pktmbuf_free(rx[0]);
    for (uint16_t pi = 0; pi < 2; pi++) { rte_eth_dev_stop(pi); rte_eth_dev_close(pi); }
    rte_eal_cleanup();
    printf("\n=== %s ===\n", ok ? "DPDK DATA PLANE OK" : "FAILED");
    return ok ? 0 : 1;
}
CEOF
    
    gcc -o /tmp/thunder_dpdk_test /tmp/thunder_dpdk_test.c \
        -include rte_config.h -march=corei7 -mrtm -mssse3 \
        -I${DPDK_ROOT}/usr/include/dpdk \
        -I${DPDK_ROOT}/usr/include/x86_64-linux-gnu/dpdk \
        -I${DPDK_ROOT}/usr/include/x86_64-linux-gnu \
        -I${DPDK_ROOT}/usr/include \
        -L${DPDK_LIB} -Wl,-rpath,${DPDK_LIB} \
        -lrte_eal -lrte_ethdev -lrte_mbuf -lrte_mempool -lrte_ring \
        -lrte_net -lrte_hash -lrte_kvargs -lrte_log -lrte_pci -lrte_bus_pci \
        -lrte_bus_vdev -lrte_timer -lrte_rcu -lpthread -ldl -lnuma 2>&1 || {
        fail "编译失败"; return 1;
    }
    
    echo "$SUDO_PASS" | sudo -S bash -c "LD_LIBRARY_PATH=${DPDK_LIB} /tmp/thunder_dpdk_test \
        --no-pci -l 0 -n 1 \
        -d ${DPDK_PMD}/librte_mempool_ring.so \
        -d ${DPDK_PMD}/librte_net_ring.so \
        --vdev=net_ring0 --vdev=net_ring1" 2>&1
}

cleanup() {
    echo "  清理 hugepages..."
    echo "$SUDO_PASS" | sudo -S sysctl -w vm.nr_hugepages=0 >/dev/null 2>&1
    echo "$SUDO_PASS" | sudo -S umount /mnt/huge 2>/dev/null || true
    echo "$SUDO_PASS" | sudo -S rm -f /dev/hugepages/rtemap_* /dev/hugepages/fbarray_* 2>/dev/null || true
    ok "已清理"
}

case "${1:-init}" in
    init)   check_sudo; setup_hugepages; setup_memlock; check_dpdk; export_env; show_status;;
    status) show_status;;
    clean)  cleanup;;
    test)   check_sudo; setup_hugepages; setup_memlock; run_test;;
    *)      echo "用法: $0 {init|status|clean|test}"; exit 1;;
esac
