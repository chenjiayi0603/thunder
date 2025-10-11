/*
 * process_helper.h
 *
 *      Author: cjy
 */

#ifndef PROCESS_HELPER_H_
#define PROCESS_HELPER_H_

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#include <sys/resource.h>

#include <stdint.h>
#include <string>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>

#define BUF_SIZE 1024

#ifdef __cplusplus
extern "C" {
#endif

void InstallSignal();
int process_daemonize(const char *dir);
int x_sock_set_block(int sock, int on);
int send_fd(int sock_fd, int send_fd) ;
int recv_fd(const int sock_fd);
int send_fd_with_attr(int sock_fd, int send_fd, void* addr, int addr_len, int send_fd_attr);
int recv_fd_with_attr(const int sock_fd, void* addr, int addr_len, int* send_fd_attr);
int readable_timeo(int fd, int sec);

//cat /proc/16184/status 	VmRSS:      8556 kB
//cat /proc/16184/stat  8761344
size_t program_get_rss_pid(pid_t pid);
size_t program_get_rss(void);

#ifdef __cplusplus
}
#endif

#endif /* PROCESS_HELPER_H_ */
