#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <ff_api.h>
#include <ff_epoll.h>

static volatile int running = 1;
static void sig_handler(int sig) { (void)sig; running = 0; }

int main(int argc, char **argv)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* F-Stack init (calls rte_eal_init internally) */
    ff_init(argc, argv);
    printf("[F-Stack] ff_init OK\n");

    int listen_fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("ff_socket"); return 1; }
    int on = 1;
    ff_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(9999);
    addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (ff_bind(listen_fd, (struct linux_sockaddr *)&addr4, sizeof(addr4)) < 0) {
        perror("ff_bind"); ff_close(listen_fd); return 1;
    }
    if (ff_listen(listen_fd, 128) < 0) {
        perror("ff_listen"); ff_close(listen_fd); return 1;
    }
    printf("[F-Stack] Listening 127.0.0.1:9999\n");

    int epfd = ff_epoll_create(1024);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    ff_epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    uint64_t rx = 0;
    printf("[F-Stack] Echo loop (Ctrl-C to stop)\n");

    while (running) {
        struct epoll_event events[256];
        int n = ff_epoll_wait(epfd, events, 256, 1);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                struct sockaddr_in ca; socklen_t len = sizeof(ca);
                int cfd = ff_accept(listen_fd, (struct linux_sockaddr *)&ca, &len);
                if (cfd >= 0) {
                    struct epoll_event cev;
                    cev.events = EPOLLIN; cev.data.fd = cfd;
                    ff_epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                    printf("[F-Stack] Accept fd=%d\n", cfd);
                }
            } else {
                char buf[4096];
                int rn = ff_read(fd, buf, sizeof(buf));
                if (rn > 0) { ff_write(fd, buf, rn); rx++; }
                else if (rn == 0) {
                    ff_epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    ff_close(fd);
                    printf("[F-Stack] Close fd=%d\n", fd);
                }
            }
        }
        if (rx > 0 && rx % 100 == 0) { printf("\r  rx=%lu", rx); fflush(stdout); }
    }
    printf("\n[F-Stack] Done. rx=%lu\n", rx);
    ff_close(listen_fd);
    close(epfd);
    return 0;
}
