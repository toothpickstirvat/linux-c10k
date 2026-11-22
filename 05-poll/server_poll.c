// 第 5 节：poll 版 echo 服务器
//
// 用法：./server_poll [swap|mark]
//   swap（默认）：删除连接时把最后一项挪过来填空，数组始终紧凑
//   mark        ：删除连接时只把 fd 设成 -1（poll 会跳过负数 fd），新连接优先复用这些空位
//
// 和 select 的区别：
//   1. 没有 FD_SETSIZE=1024 的上限，连接数只受 ulimit -n 和内存限制
//   2. events（我们关心什么）和 revents（内核报告什么）是两个字段，poll 不改写 events，
//      所以数组不用像 fd_set 那样每轮重建
//   3. 每轮仍然是 O(n)：内核要扫描整个数组，我们也要扫描整个数组找就绪的项
//
// fds[0] 固定是 listenfd，客户端从 fds[1] 开始。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, SIGPIPE
#include <unistd.h>     // read, write, close
#include <poll.h>       // poll, struct pollfd, POLLIN...
#include <sys/socket.h> // socket, bind, listen, accept4, setsockopt
#include <netinet/in.h> // struct sockaddr_in, htons, htonl, INADDR_ANY
#include <arpa/inet.h>  // inet_ntop

#define INIT_CAP 16

static struct pollfd *fds; // 可增长的数组，fds[0] 是 listenfd
static nfds_t nfds = 0;    // 已使用的项数（含 fds[0] 和 mark 模式下的空位）
static nfds_t cap = 0;     // 数组容量
static int nclient = 0;    // 真正的客户端连接数（不含空位）
static int use_mark = 0;   // 0=swap，1=mark

// 非阻塞 fd 的 write_all，EAGAIN 时原地重试（简化做法，第 6 节再改成等可写）
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

// 把新连接放进数组。成功返回 0；realloc 失败返回 -1（原数组保持有效）
static int add_fd(int fd) {
    if (use_mark) {
        // mark 模式：先找 fd == -1 的空位复用，否则空洞会越积越多
        for (nfds_t i = 1; i < nfds; i++) {
            if (fds[i].fd == -1) {
                fds[i].fd = fd;
                fds[i].events = POLLIN;
                fds[i].revents = 0;
                return 0;
            }
        }
    }
    if (nfds == cap) {
        // 不能写成 fds = realloc(fds, ...)：失败返回 NULL 时原指针会丢，造成泄漏
        // realloc 可能把数组搬到新地址，所以全程只用下标，不保存 &fds[i]
        struct pollfd *p = realloc(fds, cap * 2 * sizeof(*p));
        if (p == NULL) {
            return -1;
        }
        fds = p;
        cap *= 2;
    }
    fds[nfds].fd = fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0; // 新内存里是随机值，必须清零
    nfds++;
    return 0;
}

// 关闭并删除第 i 项（i >= 1）。调用者必须倒着遍历
static void remove_at(nfds_t i) {
    close(fds[i].fd);
    nclient--;
    if (use_mark) {
        fds[i].fd = -1; // poll 会跳过负数 fd；revents 也会被内核置 0
        // 顺手把末尾的空位收回来，数组才可能变短
        while (nfds > 1 && fds[nfds - 1].fd == -1) {
            nfds--;
        }
    } else {
        fds[i] = fds[--nfds]; // 把最后一项挪过来；倒着遍历不会漏掉它
    }
}

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        if (strcmp(argv[1], "mark") == 0) {
            use_mark = 1;
        } else if (strcmp(argv[1], "swap") != 0) {
            fprintf(stderr, "用法：%s [swap|mark]\n", argv[0]);
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

    cap = INIT_CAP;
    fds = malloc(cap * sizeof(*fds));
    if (fds == NULL) {
        perror("malloc");
        exit(1);
    }
    fds[0].fd = listenfd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    nfds = 1;

    printf("监听 0.0.0.0:9000（删除方式：%s）\n", use_mark ? "mark" : "swap");
    fflush(stdout);

    char buf[1024];
    while (1) {
        // 1. 睡在这里。数组不用重建：events 没变，revents 由内核每轮重新填写
        int n = poll(fds, nfds, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            exit(1);
        }

        // 2. 监听 socket 可读：有新连接。新项追加在末尾，revents=0，本轮不会被当成就绪
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in peer;
            socklen_t len = sizeof(peer);
            int connfd = accept4(listenfd, (struct sockaddr *)&peer, &len, SOCK_NONBLOCK);
            if (connfd < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    // EMFILE 时连接留在队列里，listenfd 一直可读，会空转（见第 4 节 README）
                    perror("accept4");
                }
            } else if (add_fd(connfd) == -1) {
                fprintf(stderr, "realloc 失败，拒绝 fd=%d\n", connfd);
                close(connfd);
            } else {
                nclient++;
                if (nclient <= 5 || nclient % 100 == 0) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                    printf("客户端 %s:%u 已连接（fd=%d，连接数 %d，nfds=%lu，cap=%lu）\n",
                           ip, ntohs(peer.sin_port), connfd, nclient, (unsigned long)nfds, (unsigned long)cap);
                    fflush(stdout);
                }
            }
        }

        // 3. 处理就绪的客户端。倒着遍历，到 1 为止（不能删掉 fds[0]）
        for (nfds_t i = nfds - 1; i >= 1; i--) {
            short re = fds[i].revents;
            if (re == 0) {
                continue; // 没事件（mark 模式的空位 fd=-1，revents 也是 0）
            }
            if (re & POLLNVAL) {
                // fd 无效：说明已关闭的 fd 还留在数组里，是代码 bug
                fprintf(stderr, "POLLNVAL：fd=%d 无效\n", fds[i].fd);
                remove_at(i);
                continue;
            }
            // 对方断开时可能只有 POLLIN（read 返回 0），也可能带着 POLLHUP/POLLERR，
            // 统一交给 read 判断，不要只检查 POLLIN
            ssize_t r = read(fds[i].fd, buf, sizeof(buf));
            if (r > 0) {
                if (write_all(fds[i].fd, buf, (size_t)r) == 0) {
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
            remove_at(i);
            if (nclient < 5 || nclient % 100 == 0) {
                printf("连接断开（连接数 %d，nfds=%lu）\n", nclient, (unsigned long)nfds);
                fflush(stdout);
            }
        }
    }
}
