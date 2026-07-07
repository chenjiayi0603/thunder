/* Thunder DPDK 数据面性能测试
 * 使用 rte_eth_from_rings() API 创建真正交叉连接的 ring pair
 * 编译: 见 run_dpdk_perf_test.sh
 * 运行: sudo LD_LIBRARY_PATH=... ./dpdk_perf_final --no-pci -l 0 -n 1 -d .../librte_mempool_ring.so
 */
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_version.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_eth_ring.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <stdio.h>
#include <string.h>

static inline uint64_t us_now(void) {
    return rte_get_timer_cycles() * 1000000UL / rte_get_timer_hz();
}

int main(int argc, char **argv) {
    rte_eal_init(argc, argv);

    struct rte_ring *r01 = rte_ring_create("ring_0to1", 1024,
        rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    struct rte_ring *r10 = rte_ring_create("ring_1to0", 1024,
        rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    struct rte_ring *p0rx[1] = {r10}, *p0tx[1] = {r01};
    struct rte_ring *p1rx[1] = {r01}, *p1tx[1] = {r10};
    int p0 = rte_eth_from_rings("thunder_p0", p0rx,1, p0tx,1, rte_socket_id());
    int p1 = rte_eth_from_rings("thunder_p1", p1rx,1, p1tx,1, rte_socket_id());

    struct rte_mempool *mp = rte_pktmbuf_pool_create("pool", 32767, 256, 0, 2176, rte_socket_id());
    for (uint16_t pi = 0; pi < 2; pi++) {
        struct rte_eth_conf c = {0}; rte_eth_dev_configure(pi, 1, 1, &c);
        rte_eth_rx_queue_setup(pi, 0, 128, rte_socket_id(), NULL, mp);
        rte_eth_tx_queue_setup(pi, 0, 128, rte_socket_id(), NULL);
        rte_eth_dev_start(pi);
    }

    printf("\nThunder DPDK 数据面性能验证\nDPDK %s | hp=%d | CPU=%lu MHz\n\n",
           rte_version(), rte_eal_has_hugepages(), rte_get_timer_hz()/1000000);

    // 功能验证
    const char *msg = "THUNDER_DPDK_DATA_PLANE_OK"; int mlen = strlen(msg)+1;
    for (int dir = 0; dir < 2; dir++) {
        int txp = dir, rxp = 1-dir;
        struct rte_mbuf *t = rte_pktmbuf_alloc(mp);
        memcpy(rte_pktmbuf_append(t, mlen), msg, mlen);
        rte_eth_tx_burst(txp, 0, &t, 1);
        struct rte_mbuf *r[32]; uint16_t n=0;
        for(int p=0;p<2000&&n==0;p++) n=rte_eth_rx_burst(rxp,0,r,32);
        printf("%d→%d: %s (recv=%u)\n", txp, rxp,
               (n>0&&memcmp(rte_pktmbuf_mtod(r[0],char*),msg,mlen)==0)?"OK":"FAIL", n);
        if(n>0) rte_pktmbuf_free(r[0]);
    }

    // 吞吐量
    printf("\n吞吐量 (0→1, 1024B, burst=32):\n");
    enum { TOTAL=10000, PL=1024, BURST=32 };
    uint64_t t0=us_now(), sent=0, recv=0;
    for(int i=0; i<TOTAL; i+=BURST) {
        int nb=(TOTAL-i)<BURST?(TOTAL-i):BURST;
        struct rte_mbuf *b[BURST]; int ready=0;
        for(int j=0;j<nb;j++){b[j]=rte_pktmbuf_alloc(mp);if(!b[j])break;rte_pktmbuf_append(b[j],PL);ready++;}
        if(!ready)break;
        uint16_t ns=rte_eth_tx_burst(0,0,b,ready); sent+=ns;
        for(int j=ns;j<ready;j++)rte_pktmbuf_free(b[j]);
        struct rte_mbuf *r[BURST];
        uint16_t nr=rte_eth_rx_burst(1,0,r,BURST);
        for(int j=0;j<nr;j++)rte_pktmbuf_free(r[j]); recv+=nr;
    }
    for(int d=0;d<5000;d++){struct rte_mbuf *r[BURST];uint16_t n=rte_eth_rx_burst(1,0,r,BURST);if(!n)break;for(int j=0;j<n;j++)rte_pktmbuf_free(r[j]);recv+=n;}
    double sec=(us_now()-t0)/1e6;
    printf("  sent=%lu recv=%lu loss=%ld Mbps=%.0f PPS=%.0f\n", sent, recv, sent-recv,
           sent*PL*8.0/(sec*1e6), sent/sec);

    // 延迟
    printf("\n延迟 (0→1 ping-pong, 128B):\n");
    enum { N=2000 }; uint64_t lats[N]; int cnt=0;
    for(int i=0;i<N;i++){
        struct rte_mbuf *p=rte_pktmbuf_alloc(mp); rte_pktmbuf_append(p,128);
        uint64_t tt0=us_now(); rte_eth_tx_burst(0,0,&p,1);
        struct rte_mbuf *q[1];uint16_t n=0;
        for(int w=0;w<500&&n==0;w++)n=rte_eth_rx_burst(1,0,q,1);
        if(n>0){lats[cnt++]=us_now()-tt0; rte_pktmbuf_free(q[0]);} else rte_pktmbuf_free(p);
    }
    if(cnt>0){uint64_t mn=lats[0],mx=lats[0];double sum=0;
        for(int i=0;i<cnt;i++){if(lats[i]<mn)mn=lats[i];if(lats[i]>mx)mx=lats[i];sum+=lats[i];}
        printf("  %d/%d | min=%luus avg=%.1fus max=%luus\n", cnt,N,mn,sum/cnt,mx);
    }

    for(uint16_t pi=0;pi<2;pi++){rte_eth_dev_stop(pi);rte_eth_dev_close(pi);}
    rte_eal_cleanup();
    return 0;
}
