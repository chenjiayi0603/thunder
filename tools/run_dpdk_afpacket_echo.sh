#!/usr/bin/env bash
# Thunder DPDK AF_PACKET PMD 可行性验证
# 用法: sudo ./tools/run_dpdk_afpacket_echo.sh
set -euo pipefail

DPDK_ROOT="${HOME}/.local/dpdk"
DPDK_LIB="${DPDK_ROOT}/usr/lib/x86_64-linux-gnu"
DPDK_PMD="${DPDK_LIB}/dpdk/pmds-26.0"
DPDK_INC="${DPDK_ROOT}/usr/include/dpdk"
DPDK_INC_X86="${DPDK_ROOT}/usr/include/x86_64-linux-gnu/dpdk"

echo "=== 1. hugepages ==="
sudo sysctl -w vm.nr_hugepages=256

echo "=== 2. 编译 ==="
SOURCE="$(dirname "$0")/dpdk_afpacket_echo.c"
gcc -O2 -o /tmp/dpdk_afpacket_echo "$SOURCE" \
    -include rte_config.h -march=corei7 -mssse3 -mrtm \
    -I${DPDK_INC} -I${DPDK_INC_X86} \
    -I${DPDK_ROOT}/usr/include/x86_64-linux-gnu \
    -I${DPDK_ROOT}/usr/include \
    -L${DPDK_LIB} -Wl,-rpath,${DPDK_LIB} \
    -lrte_eal -lrte_ethdev -lrte_mbuf -lrte_mempool -lrte_ring \
    -lrte_net -lrte_hash -lrte_kvargs -lrte_log -lrte_pci -lrte_bus_pci \
    -lrte_bus_vdev -lrte_timer -lrte_rcu -lpthread -ldl -lnuma

echo "=== 3. 启动 AF_PACKET Echo (Ctrl-C 停止) ==="
LD_LIBRARY_PATH=${DPDK_LIB} /tmp/dpdk_afpacket_echo \
    --no-pci -l 0 -n 1 \
    --vdev=net_af_packet0,iface=lo \
    -d ${DPDK_PMD}/librte_mempool_ring.so \
    -d ${DPDK_PMD}/librte_net_af_packet.so

echo "=== 4. 清理 ==="
sudo sysctl -w vm.nr_hugepages=0
