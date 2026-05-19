#!/usr/bin/env bash
# Thunder DPDK 数据面性能测试 — 一键脚本
# 用法: ./tools/run_dpdk_perf_test.sh
set -euo pipefail

DPDK_ROOT="${HOME}/.local/dpdk"
DPDK_LIB="${DPDK_ROOT}/usr/lib/x86_64-linux-gnu"
DPDK_PMD="${DPDK_LIB}/dpdk/pmds-26.0"
DPDK_INC="${DPDK_ROOT}/usr/include/dpdk"
DPDK_INC_X86="${DPDK_ROOT}/usr/include/x86_64-linux-gnu/dpdk"
SUDO_PASS="${SUDO_PASS:-1q2w3e}"

# 编译
SOURCE="${0%.sh}.c"
[ -f "$SOURCE" ] || { echo "请将 dpdk_perf_final 的 C 源码放到 $SOURCE"; exit 1; }

gcc -O2 -o /tmp/dpdk_perf_final "$SOURCE" \
    -include rte_config.h -march=corei7 -mrtm -mssse3 \
    -I${DPDK_INC} -I${DPDK_INC_X86} \
    -I${DPDK_ROOT}/usr/include/x86_64-linux-gnu \
    -I${DPDK_ROOT}/usr/include \
    -L${DPDK_LIB} -Wl,-rpath,${DPDK_LIB} \
    -lrte_eal -lrte_ethdev -lrte_mbuf -lrte_mempool -lrte_ring \
    -lrte_net -lrte_hash -lrte_kvargs -lrte_log -lrte_pci -lrte_bus_pci \
    -lrte_bus_vdev -lrte_timer -lrte_rcu -lpthread -ldl -lnuma \
    ${DPDK_LIB}/librte_net_ring.so

# 运行
echo "$SUDO_PASS" | sudo -S rm -rf /var/run/dpdk/* 2>/dev/null
echo "$SUDO_PASS" | sudo -S bash -c "LD_LIBRARY_PATH=${DPDK_LIB} /tmp/dpdk_perf_final \
    --no-pci -l 0 -n 1 \
    -d ${DPDK_PMD}/librte_mempool_ring.so"
