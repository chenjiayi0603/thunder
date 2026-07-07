/*
 * DPDK KNI + TCP 可达性测试 — Step 3 替代方案
 *
 * F-Stack 编译失败。改用 DPDK KNI:
 *   DPDK AF_PACKET 收帧 → KNI 桥接 → 内核 TCP 栈 → curl 直通
 *
 * 架构:
 *   curl → 127.0.0.1:9999 → 内核 TCP → KNI → DPDK → AF_PACKET → lo → 回来
 *
 * 不需要 F-Stack。不需要用户态 TCP 栈。
 * 本机 DPDK 25.11 即可。
 *
 * 编译 (用本机 DPDK 25.11):
 *   gcc -O2 -o dpdk_kni_test dpdk_kni_tcp_test.c \
 *       -include rte_config.h -march=corei7 -mssse3 -mrtm
 *       -I~/.local/dpdk/usr/include/dpdk \
 *       -L~/.local/dpdk/usr/lib/x86_64-linux-gnu \
 *       [DPDK libs...]
 *
 * 运行:
 *   sudo ./dpdk_kni_test --no-pci -l 0 -n 1 \
 *     --vdev=net_af_packet0,iface=lo \
 *     -d dpdk/pmds-26.0/librte_mempool_ring.so \
 *     -d dpdk/pmds-26.0/librte_net_af_packet.so
 *
 * 另一个终端:
 *   sudo ip addr add 10.99.0.2/24 dev kni0
 *   sudo ip link set kni0 up
 *   nc -l 9999  # 或 python -m http.server
 *
 * 或反过来: KNI 端口绑定到 lo, 本机 curl 127.0.0.1:9999 就能通
 */

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_kni.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int running = 1;
static void sig_handler(int sig) { (void)sig; running = 0; }

/* KNI 配置: 把 DPDK port 0 桥接到内核 kni0 接口 */
static struct rte_kni_conf kni_conf = {
    .name        = "kni0",
    .group_id    = 0,
    .mbuf_size   = RTE_MBUF_DEFAULT_BUF_SIZE,
};

int main(int argc, char **argv)
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL init failed\n");

    int nb_ports = rte_eth_dev_count_avail();
    printf("\n=== DPDK KNI Bridge Test ===\n");
    printf("Ports: %d | hp: %d\n", nb_ports, rte_eal_has_hugepages());

    if (nb_ports < 1)
        rte_exit(EXIT_FAILURE, "No ports. Add --vdev=net_af_packet0,iface=lo\n");

    uint16_t port_id = 0;

    /* Mempool (KNI 需要独立的 mempool) */
    struct rte_mempool *mp = rte_pktmbuf_pool_create(
        "kni_pool", 8191, 256, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!mp)
        rte_exit(EXIT_FAILURE, "mempool failed\n");

    struct rte_eth_conf port_conf = {0};
    rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    rte_eth_rx_queue_setup(port_id, 0, 128, rte_socket_id(), NULL, mp);
    rte_eth_tx_queue_setup(port_id, 0, 128, rte_socket_id(), NULL);
    rte_eth_dev_start(port_id);

    /* 获取 port MAC 给 KNI */
    struct rte_ether_addr mac;
    rte_eth_macaddr_get(port_id, &mac);
    memcpy(kni_conf.mac_addr.addr_bytes, mac.addr_bytes, 6);

    /* 创建 KNI */
    struct rte_kni *kni = rte_kni_alloc(mp, &kni_conf, NULL);
    if (!kni)
        rte_exit(EXIT_FAILURE, "KNI alloc failed "
                 "(need rte_kni.ko loaded? try: sudo insmod dpdk/kni/rte_kni.ko)\n");

    printf("KNI: kni0 created\n");
    printf("Bridge: port %u <-> kni0 <-> kernel TCP stack\n", port_id);
    printf("\nIn another terminal:\n");
    printf("  sudo ip addr add 10.99.0.2/24 dev kni0 && sudo ip link set kni0 up\n");
    printf("  nc -l 10.99.0.2 9999\n");
    printf("\nThen:\n");
    printf("  echo test | nc 10.99.0.2 9999\n");
    printf("\nBridge running (Ctrl-C to stop)...\n\n");

    uint64_t rx_total = 0, tx_total = 0;

    while (running) {
        /* DPDK → KNI (收包到内核) */
        struct rte_mbuf *rx_bufs[32];
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_bufs, 32);
        if (nb_rx > 0) {
            uint16_t nb_kni = rte_kni_tx_burst(kni, rx_bufs, nb_rx);
            /* 未发给 KNI 的 mbuf 需释放 */
            for (uint16_t i = nb_kni; i < nb_rx; i++)
                rte_pktmbuf_free(rx_bufs[i]);
            rx_total += nb_kni;
        }

        /* KNI → DPDK (内核发包出去) */
        struct rte_mbuf *tx_bufs[32];
        uint16_t nb_kni_tx = rte_kni_rx_burst(kni, tx_bufs, 32);
        if (nb_kni_tx > 0) {
            uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, tx_bufs, nb_kni_tx);
            for (uint16_t i = nb_tx; i < nb_kni_tx; i++)
                rte_pktmbuf_free(tx_bufs[i]);
            tx_total += nb_tx;
        }

        if ((rx_total + tx_total) % 1000 == 0 && (rx_total + tx_total) > 0) {
            printf("\r  rx=%lu tx=%lu", rx_total, tx_total);
            fflush(stdout);
        }
        rte_delay_us(100);
    }

    printf("\n=== Done: rx=%lu tx=%lu ===\n", rx_total, tx_total);
    rte_kni_release(kni);
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_eal_cleanup();
    return 0;
}
