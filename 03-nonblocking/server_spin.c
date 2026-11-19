// 第 3 节：非阻塞轮询版 echo 服务器（反面教材）
//
// 用法：./server_spin [每轮休眠微秒数]
//   ./server_spin          不休眠，空闲时也会把一个 CPU 核吃满
//   ./server_spin 1000     每轮循环后 usleep(1000)，CPU 降下来，但每个请求最多多等 1ms
//
// 只用一个线程服务所有连接：监听 socket 和所有连接 socket 都设成非阻塞，
// 主循环不停地"试一次 accept，再挨个试一次 read"。没数据时 read 立刻返回 EAGAIN，
// 所以线程从不阻塞，但也从不休息：绝大部分系统调用都是白跑一趟。
// 下一节的 select 就是来解决这个问题的：让内核告诉我们哪个 fd 有事，没事就睡。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, SIGPIPE
#include <unistd.h>     // read, write, close, usleep
#include <sys/socket.h> // socket, bind, listen, accept4, setsockopt
#include <netinet/in.h> // struct sockaddr_in, htons, htonl, INADDR_ANY
#include <arpa/inet.h>  // inet_ntop

#define MAX_CONN 1024 // 最多同时服务的连接数

// 一个连接的状态。轮询版只需要 fd 和对方地址（用来打印）。
typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
    unsigned port;
} conn_t;

static conn_t conns[MAX_CONN];
static int nconn = 0; // conns[0..nconn) 是正在服务的连接

// 非阻塞版write_all。
// 非阻塞socket的发送缓冲区满了时，write返回-1、errno = EAGAIN，表示"现在写不进去"，不是出错。
// 正确的做法是把没写完的数据存起来，等fd可写了再写（第6节epoll会做）。
// 这里为了简单，遇到EAGAIN就原地重试，也就是在写这一步忙等。echo的数据量很小，几乎碰不到。
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

// 关闭第i个连接，并把最后一个连接挪到第i个位置（数组保持紧凑，O(1)删除）
static void remove_conn(int i) {
    close(conns[i].fd);
    conns[i] = conns[nconn - 1];
    nconn--;
}

int main(int argc, char *argv[]) {
    long sleep_us = 0;
    if (argc >= 2) {
        char *end;
        sleep_us = strtol(argv[1], &end, 10);
        if (*end != '\0' || sleep_us < 0) {
            fprintf(stderr, "用法：%s [每轮休眠微秒数]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    // SOCK_NONBLOCK：创建时直接设成非阻塞，省一次fcntl。
    // 等价写法：fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
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
    printf("监听 0.0.0.0:9000（每轮休眠 %ld 微秒）\n", sleep_us);
    fflush(stdout);

    char buf[1024];
    while (1) {
        // 第1步：试一次accept。队列里没有连接时，非阻塞的accept立刻返回EAGAIN。
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        // accept4 + SOCK_NONBLOCK：新连接的fd直接就是非阻塞的。
        // 注意：listenfd是非阻塞的，不代表accept出来的connfd也是，必须单独设置。
        int connfd = accept4(listenfd, (struct sockaddr *)&peer, &len, SOCK_NONBLOCK);
        if (connfd >= 0) {
            if (nconn == MAX_CONN) {
                fprintf(stderr, "连接数已满（%d），拒绝新连接\n", MAX_CONN);
                close(connfd);
            } else {
                conn_t *c = &conns[nconn++];
                c->fd = connfd;
                inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof(c->ip));
                c->port = ntohs(peer.sin_port);
                printf("客户端 %s:%u 已连接（fd=%d，当前 %d 个连接）\n", c->ip, c->port, connfd, nconn);
            }
        } else if (errno != EAGAIN && errno != EINTR) {
            // EAGAIN（== EWOULDBLOCK）：现在没有新连接，不是错误
            perror("accept4");
        }

        // 第2步：挨个试一次read。没数据的连接立刻返回EAGAIN，跳过。
        // 倒着遍历：remove_conn会把最后一个挪到位置i，倒着走不会漏掉被挪过来的那个。
        for (int i = nconn - 1; i >= 0; i--) {
            conn_t *c = &conns[i];
            ssize_t n = read(c->fd, buf, sizeof(buf));
            if (n > 0) {
                printf("<-- %s:%u: %.*s", c->ip, c->port, (int)n, buf);
                fflush(stdout);
                if (write_all(c->fd, buf, (size_t)n) == -1) {
                    perror("write");
                    remove_conn(i);
                }
                continue;
            }
            if (n == 0) {
                printf("客户端 %s:%u 断开\n", c->ip, c->port);
                fflush(stdout);
                remove_conn(i);
                continue;
            }
            // n == -1
            if (errno == EAGAIN || errno == EINTR) {
                continue; // 这个连接现在没数据，看下一个。绝大部分read都走到这里
            }
            perror("read"); // 真正的错误，比如ECONNRESET
            remove_conn(i);
        }

        // 不休眠：这一轮什么都没干，立刻进入下一轮，空转把CPU吃满。
        // 休眠：CPU降了，但一个刚到的请求最多要多等sleep_us才会被看到。
        if (sleep_us > 0) {
            usleep((useconds_t)sleep_us);
        }
    }
}
