// 第 7 节 任务 A：故意写错的 ET 服务器（反面教材）
//
// 在第 6 节 LT 版的基础上只做两处改动：
//   1. 客户端 fd 注册时加 EPOLLET（边沿触发）
//   2. 读缓冲区缩到 16 字节
// 读的逻辑故意不改：每次通知只 read 一次。
// listenfd 保持 LT，这样观察到的现象只来自“读一次”，不掺杂 accept 的问题。
//
// 实验：客户端一次发 100 字节。ET 只在数据到达时通知一次，服务器读走 16 字节后，
// 剩下的 84 字节留在内核接收缓冲区，不会再有通知，连接就卡住了（用 ss -tn 看 Recv-Q）。
// 用法：./server_epoll_et_bad

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, SIGPIPE
#include <unistd.h>     // read, write, close
#include <sys/epoll.h>  // epoll_create1, epoll_ctl, epoll_wait
#include <sys/socket.h> // socket, bind, listen, accept4, setsockopt
#include <netinet/in.h> // struct sockaddr_in, htons, htonl, INADDR_ANY

#define MAX_EVENTS 64
#define RBUF 16 // 故意很小，让一次 read 读不完

// 非阻塞 fd 的 write_all，EAGAIN 时原地重试（简化做法）
static int write_all(int fd, const char *buf, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, buf + written, size - written);
        if (n == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listenfd == -1) {
        perror("socket");
        exit(1);
    }
    int one = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
        perror("setsockopt");
        exit(1);
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        exit(1);
    }
    if (listen(listenfd, SOMAXCONN) == -1) {
        perror("listen");
        exit(1);
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
        exit(1);
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN; // listenfd 用 LT
    ev.data.fd = listenfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) {
        perror("epoll_ctl ADD listenfd");
        exit(1);
    }

    printf("监听 0.0.0.0:9000（ET 错误版：客户端 fd 用 EPOLLET，每次只读 %d 字节）\n", RBUF);
    fflush(stdout);

    struct epoll_event events[MAX_EVENTS];
    char buf[RBUF];
    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            exit(1);
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listenfd) {
                int connfd = accept4(listenfd, NULL, NULL, SOCK_NONBLOCK);
                if (connfd < 0) {
                    if (errno != EAGAIN && errno != EINTR) {
                        perror("accept4");
                    }
                    continue;
                }
                memset(&ev, 0, sizeof(ev));
                ev.events = EPOLLIN | EPOLLET; // 加了 EPOLLET
                ev.data.fd = connfd;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev) == -1) {
                    perror("epoll_ctl ADD");
                    close(connfd);
                    continue;
                }
                printf("新连接 fd=%d\n", connfd);
                fflush(stdout);
                continue;
            }

            // 错误：只 read 一次，没有读到 EAGAIN
            ssize_t r = read(fd, buf, sizeof(buf));
            printf("通知 fd=%d 事件=0x%x read=%zd\n", fd, events[i].events, r);
            fflush(stdout);
            if (r > 0) {
                if (write_all(fd, buf, (size_t)r) == 0) {
                    continue;
                }
                perror("write");
            } else if (r == -1 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            } else if (r == -1) {
                perror("read");
            }
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
        }
    }
}
