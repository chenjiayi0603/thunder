/*
 * DPDK AF_PACKET PMD 可行性验证 — L2 Echo
 *
 * 用法:
 *   sudo ./dpdk_afpacket_echo --no-pci -l 0 -n 1 \
 *     --vdev=net_af_packet0,iface=lo \
 *     -d dpdk/pmds-26.0/librte_mempool_ring.so \
 *     -d dpdk/pmds-26.0/librte_net_af_packet.so
 *
 * 不需要 F-Stack, 不需要特殊网卡, 不需要 TCP 栈。
 * 纯 L2 帧收发的 echo 服务 — 收到帧, 交换 MAC 地址, 原样发回。
 */
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int running = 1;

static void sig_handler(int sig) { (void)sig; running = 0; }

int main(int argc, char **argv)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");

    int nb_ports = rte_eth_dev_count_avail();
    printf("\n=== DPDK AF_PACKET Echo Test ===\n");
    printf("DPDK ports=%d | hugepages=%d\n",
           nb_ports, rte_eal_has_hugepages());

    if (nb_ports < 1)
        rte_exit(EXIT_FAILURE, "No DPDK ports. Add --vdev=net_af_packet0,iface=lo\n");

    uint16_t port_id = 0;

    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(port_id, &dev_info);
    printf("Port %u: driver=%s\n", port_id, dev_info.driver_name);

    struct rte_mempool *mp = rte_pktmbuf_pool_create(
        "af_pkt_pool", 8191, 256, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!mp)
        rte_exit(EXIT_FAILURE, "mempool create failed\n");
    printf("Mempool: OK\n");

    struct rte_eth_conf port_conf = {0};
    ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "port configure failed\n");

    ret = rte_eth_rx_queue_setup(port_id, 0, 128, rte_socket_id(), NULL, mp);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rx queue setup failed\n");

    ret = rte_eth_tx_queue_setup(port_id, 0, 128, rte_socket_id(), NULL);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "tx queue setup failed\n");

    ret = rte_eth_dev_start(port_id);
    if (ret < 0)
        printf("WARN: dev_start failed (AF_PACKET on lo may need traffic to init)\n");

    struct rte_ether_addr my_mac;
    rte_eth_macaddr_get(port_id, &my_mac);
    printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           my_mac.addr_bytes[0], my_mac.addr_bytes[1], my_mac.addr_bytes[2],
           my_mac.addr_bytes[3], my_mac.addr_bytes[4], my_mac.addr_bytes[5]);

    printf("\nEcho loop running... (Ctrl-C to stop, send L2 frames to lo)\n\n");

    uint64_t total_rx = 0, total_tx = 0;
    uint64_t t0 = rte_rdtsc();

    while (running)
    {
        struct rte_mbuf *rx_bufs[32];
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_bufs, 32);

        if (nb_rx == 0) {
            rte_delay_us(100);
            continue;
        }

        for (uint16_t i = 0; i < nb_rx; i++)
        {
            struct rte_mbuf *m = rx_bufs[i];
            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            struct rte_ether_addr tmp = eth->dst_addr;
            eth->dst_addr = eth->src_addr;
            eth->src_addr = tmp;
            (void)tmp;
            total_rx++;
        }

        uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, rx_bufs, nb_rx);
        for (uint16_t i = nb_tx; i < nb_rx; i++)
            rte_pktmbuf_free(rx_bufs[i]);
        total_tx += nb_tx;

        if (total_rx % 1000 == 0) {
            double sec = (rte_rdtsc() - t0) / (double)rte_get_tsc_hz();
            printf("\r  rx=%lu tx=%lu loss=%lu sec=%.1f",
                   total_rx, total_tx,
                   total_rx > total_tx ? total_rx - total_tx : 0, sec);
            fflush(stdout);
        }
    }

    double sec = (rte_rdtsc() - t0) / (double)rte_get_tsc_hz();
    printf("\n\n=== Final ===\n");
    printf("Duration: %.1f sec\n", sec);
    printf("RX: %lu  TX: %lu  Loss: %lu\n",
           total_rx, total_tx,
           total_rx > total_tx ? total_rx - total_tx : 0);
    if (sec > 0 && total_tx > 0)
        printf("PPS: %.0f\n", total_tx / sec);

    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_eal_cleanup();
    return 0;
}
