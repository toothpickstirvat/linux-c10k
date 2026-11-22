// 第 7 节 任务 B：正确的 epoll 边沿触发（ET）echo 服务器
//
// 用法：./server_epoll_et [block]
//   ./server_epoll_et         客户端 fd 非阻塞（正确做法）
//   ./server_epoll_et block   客户端 fd 故意用阻塞模式，验证思考题 1：
//                             读循环读完数据后 read 不返回 EAGAIN，直接睡死，整个服务器被卡住
//
// ET 的规则：收到通知后必须一直做到 EAGAIN，否则剩下的不会再通知：
//   1. listenfd 用 ET：accept 要循环到 EAGAIN（多个连接同时到达只通知一次）
//   2. 客户端 fd 用 ET：read 要循环到 EAGAIN
// 公平性没有处理：一个连接数据特别多时，读循环会让别的连接等待。
// 简单地限制读的次数会丢通知（剩下的数据不再触发），需要自己记下“这个 fd 还有数据”，以后再处理。

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

// 以 EPOLLIN | EPOLLET 加入 epoll
static int epoll_add_et(int epfd, int fd) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int main(int argc, char *argv[]) {
    int client_flags = SOCK_NONBLOCK;
    if (argc >= 2) {
        if (strcmp(argv[1], "block") == 0) {
            client_flags = 0; // 故意阻塞
        } else {
            fprintf(stderr, "用法：%s [block]\n", argv[0]);
            return 1;
        }
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
    if (epoll_add_et(epfd, listenfd) == -1) {
        perror("epoll_ctl ADD listenfd");
        exit(1);
    }

    printf("监听 0.0.0.0:9000（epoll ET，客户端 fd %s）\n", client_flags ? "非阻塞" : "阻塞（故意）");
    fflush(stdout);

    int nclient = 0;
    struct epoll_event events[MAX_EVENTS];
    char buf[1024];
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
                // ET：一次通知可能对应很多个等待中的连接，必须循环 accept 到 EAGAIN
                while (1) {
                    struct sockaddr_in peer;
                    socklen_t len = sizeof(peer);
                    int connfd = accept4(listenfd, (struct sockaddr *)&peer, &len, client_flags);
                    if (connfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // 队列取空了
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        perror("accept4"); // 比如 EMFILE
                        break;
                    }
                    if (epoll_add_et(epfd, connfd) == -1) {
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
                }
                continue;
            }

            // ET：循环 read 到 EAGAIN，把内核缓冲区读空
            int closed = 0;
            while (1) {
                ssize_t r = read(fd, buf, sizeof(buf));
                if (r > 0) {
                    if (write_all(fd, buf, (size_t)r) == -1) {
                        perror("write");
                        closed = 1;
                        break;
                    }
                    continue;
                }
                if (r == 0) {
                    closed = 1; // 对方关闭
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 读空了，等下一次通知
                }
                perror("read");
                closed = 1;
                break;
            }
            if (closed) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                nclient--;
                if (nclient < 5 || nclient % 1000 == 0) {
                    printf("连接断开（连接数 %d）\n", nclient);
                    fflush(stdout);
                }
            }
        }
    }
}
