// 第 1 节：阻塞 echo 服务器
//
// 监听 0.0.0.0:9000，一次只服务一个客户端：accept 一个连接后循环 read/write，
// 直到对方关闭，再回去 accept 下一个。服务当前客户端时，新来的连接只能在
// listen 的全连接队列里排队（内核已经替它完成了三次握手），数据也没人读。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, SIGPIPE
#include <unistd.h>     // read, write, close
#include <sys/socket.h> // socket, bind, listen, accept, setsockopt
#include <netinet/in.h> // struct sockaddr_in, htons, htonl, INADDR_ANY
#include <arpa/inet.h>  // inet_ntop

// 把 buf 的 size 个字节全部写出去。
// write 可能只写了一部分（short write），所以要循环，直到写完。
// 成功返回 0，出错返回 -1（errno 保留 write 的错误）。
static int write_all(int fd, const char *buf, size_t size) {
    size_t written = 0; // 已经写出去的字节数
    while (written < size) {
        ssize_t n = write(fd, buf + written, size - written);
        if (n == -1) {
            if (errno == EINTR) { // 被信号打断，什么都没写，重试
                continue;
            }
            return -1; // 其他错误，比如 EPIPE（对方已关闭）
        }
        written += (size_t)n; // 累加，下一次从没写完的位置继续
    }
    return 0;
}

int main(void) {
    // 对方关闭后再 write，内核会发 SIGPIPE，默认动作是直接杀死进程。
    // 忽略它之后，write 改为返回 -1、errno = EPIPE，由 write_all 正常处理。
    signal(SIGPIPE, SIG_IGN);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        exit(1);
    }

    // 允许重启后立刻重新 bind 同一个端口，
    // 否则上次的连接处于 TIME_WAIT 时会报 "Address already in use"
    int one = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);              // 端口转成网络字节序（大端）
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0，监听所有网卡
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        exit(1);
    }

    // 从这里开始，内核会替我们完成三次握手，把建立好的连接放进队列，
    // 队列长度上限是 backlog（还受 /proc/sys/net/core/somaxconn 限制）
    if (listen(sockfd, SOMAXCONN) == -1) {
        perror("listen");
        exit(1);
    }
    printf("监听 0.0.0.0:9000\n");

    while (1) { // 外层：一个接一个地服务客户端
        struct sockaddr_in peer_addr;
        // addrlen 既是输入（缓冲区多大）也是输出（实际写了多少），每次都要重新赋值
        socklen_t addrlen = sizeof(peer_addr);
        // 队列里没有连接时，accept 阻塞在这里
        int connfd = accept(sockfd, (struct sockaddr *)&peer_addr, &addrlen);
        if (connfd == -1) {
            // 单个连接出错不影响服务器，继续等下一个
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer_addr.sin_addr, ip, sizeof(ip));
        unsigned port = ntohs(peer_addr.sin_port);
        printf("客户端 %s:%u 已连接（fd=%d）\n", ip, port, connfd);

        char buf[1024];
        while (1) { // 内层：服务当前这个客户端
            // 对方没发数据时，read 阻塞在这里；这期间别的客户端都没人管
            ssize_t n = read(connfd, buf, sizeof(buf));
            if (n > 0) {
                // buf 不以 '\0' 结尾，也可能带着上次的残留，只打印本次读到的 n 个字节
                printf("<-- %s:%u: %.*s", ip, port, (int)n, buf);
                if (write_all(connfd, buf, (size_t)n) == -1) {
                    perror("write");
                    break;
                }
                continue;
            }
            if (n == 0) { // 对方关闭了连接（收到 FIN）
                printf("客户端 %s:%u 断开\n", ip, port);
                break;
            }
            // n == -1
            if (errno == EINTR) {
                continue;
            }
            perror("read"); // 比如 ECONNRESET：对方发了 RST
            break;
        }
        close(connfd); // 每个 fd 只在这一个地方关
    }

    close(sockfd);
    return 0;
}
