// 第 6 节：epoll 水平触发（LT）echo 服务器
//
// 用法：./server_epoll_lt [每次读取字节数] [v]
//   ./server_epoll_lt           每次最多 read 1024 字节，不打印 epoll_wait 日志
//   ./server_epoll_lt 1         任务 C：每次只 read 1 字节，观察 LT 重复通知
//   ./server_epoll_lt 1024 v    任务 B：每次 epoll_wait 返回都打印返回值（连接多时会刷屏）
//
// 和 poll 的区别：监视集合只在增删时用 epoll_ctl 告诉内核一次，
// epoll_wait 只返回就绪的 fd，开销和就绪个数有关，和总连接数无关。
// 不再需要自己维护连接数组：fd 存在内核里，没有 realloc，也没有挪元素的问题。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, SIGPIPE
#include <unistd.h>     // read, write, close
#include <sys/epoll.h>  // epoll_create1, epoll_ctl, epoll_wait
#include <sys/socket.h> // socket, bind, listen, accept4, setsockopt
#include <netinet/in.h> // struct sockaddr_in, htons, htonl, INADDR_ANY
#include <arpa/inet.h>  // inet_ntop

#define MAX_EVENTS 64

// 非阻塞 fd 的 write_all，EAGAIN 时原地重试（简化做法，下一节再改成等 EPOLLOUT）
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

// 把 fd 以 EPOLLIN 加入 epoll。成功返回 0
static int epoll_add(int epfd, int fd) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

// 关闭连接：先显式 DEL，再 close。
// 只有一个 fd 号指向这个 socket 时，close 会自动把它从 epoll 里删掉；
// 但 fd 被 dup 或 fork 后别处还开着时不会（epoll 监视的是底层打开的文件，不是 fd 号），
// 所以显式 DEL 更稳妥。
static void close_conn(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

int main(int argc, char *argv[]) {
    size_t rsize = 1024;
    int verbose = 0;
    char buf[1024];
    if (argc >= 2) {
        char *end;
        long v = strtol(argv[1], &end, 10);
        if (*end != '\0' || v < 1 || v > (long)sizeof(buf)) {
            fprintf(stderr, "用法：%s [每次读取字节数 1-%zu] [v]\n", argv[0], sizeof(buf));
            return 1;
        }
        rsize = (size_t)v;
    }
    if (argc >= 3) {
        verbose = strcmp(argv[2], "v") == 0;
    }

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
    if (epoll_add(epfd, listenfd) == -1) {
        perror("epoll_ctl ADD listenfd");
        exit(1);
    }

    printf("监听 0.0.0.0:9000（epoll LT，每次读 %zu 字节，日志 %s）\n", rsize, verbose ? "开" : "关");
    fflush(stdout);

    int nclient = 0;
    struct epoll_event events[MAX_EVENTS];
    while (1) {
        // 只返回就绪的 fd，events[0..n) 有效。就绪数超过 MAX_EVENTS 时剩下的留在就绪链表里，下一轮继续返回
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            exit(1);
        }
        if (verbose) {
            printf("epoll_wait 返回 %d\n", n);
            fflush(stdout);
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listenfd) {
                // 每次只 accept 一个；LT 下队列里还有连接时，下一轮 epoll_wait 会再次报告
                struct sockaddr_in peer;
                socklen_t len = sizeof(peer);
                int connfd = accept4(listenfd, (struct sockaddr *)&peer, &len, SOCK_NONBLOCK);
                if (connfd < 0) {
                    if (errno != EAGAIN && errno != EINTR) {
                        perror("accept4"); // EMFILE 时 listenfd 会一直可读，空转（见第 4 节 README）
                    }
                    continue;
                }
                if (epoll_add(epfd, connfd) == -1) {
                    perror("epoll_ctl ADD");
                    close(connfd);
                    continue;
                }
                nclient++;
                if (nclient <= 5 || nclient % 1000 == 0) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                    printf("客户端 %s:%u 已连接（fd=%d，连接数 %d）\n", ip, ntohs(peer.sin_port), connfd, nclient);
                    fflush(stdout);
                }
                continue;
            }

            // 客户端 fd：只要有事件（EPOLLIN/EPOLLHUP/EPOLLERR）就交给 read 判断
            ssize_t r = read(fd, buf, rsize);
            if (verbose) {
                printf("  fd=%d 事件=0x%x read=%zd\n", fd, events[i].events, r);
                fflush(stdout);
            }
            if (r > 0) {
                if (write_all(fd, buf, (size_t)r) == 0) {
                    continue;
                }
                perror("write");
            } else if (r == 0) {
                // 对方正常关闭
            } else if (errno == EAGAIN || errno == EINTR) {
                continue;
            } else {
                perror("read");
            }
            close_conn(epfd, fd);
            nclient--;
            if (nclient < 5 || nclient % 1000 == 0) {
                printf("连接断开（连接数 %d）\n", nclient);
                fflush(stdout);
            }
        }
    }
}
