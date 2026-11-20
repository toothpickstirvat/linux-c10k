// 第 4 节：select 版 echo 服务器
//
// 单线程服务所有连接。线程睡在 select 里，任何一个 fd 可读时才醒来，空闲时不占 CPU。
// 每一轮：根据 clients[] 重新生成 fd_set → select → 处理新连接 → 处理就绪的客户端。
//
// 故意没有检查 fd >= FD_SETSIZE（1024），任务 B 要撞这堵墙：
//   server_select          默认编译（FORTIFY 开启），FD_SET 越界时程序直接终止
//   server_select_nofortify 加了 -U_FORTIFY_SOURCE，越界写会悄悄破坏栈上的其他变量

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, SIGPIPE
#include <unistd.h>     // read, write, close
#include <sys/socket.h> // socket, bind, listen, accept4, setsockopt
#include <netinet/in.h> // struct sockaddr_in, htons, htonl, INADDR_ANY
#include <arpa/inet.h>  // inet_ntop
#include <sys/select.h> // select, fd_set, FD_ZERO, FD_SET, FD_ISSET, FD_SETSIZE

// 连接数组的容量故意比 FD_SETSIZE 大：任务 B 要看到的是 FD_SET 越界，而不是这个数组先满
#define MAX_CLIENTS 4096

// 把 buf 的 size 个字节全部写出去。fd 是非阻塞的，发送缓冲区满时 write 返回 EAGAIN，
// 这里简化成原地重试（echo 数据量小，几乎碰不到）；正确做法是存起来等可写再写（第 6 节）。
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

    // listenfd 也设成非阻塞：select 说可读之后、accept 之前，客户端可能已经发了 RST，
    // 队列又空了，阻塞的 accept 会把整个服务器卡住
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }
    int one = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
        perror("setsockopt");
        exit(1);
    }
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(9000);
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(sockfd, SOMAXCONN) < 0) {
        perror("listen");
        exit(1);
    }
    printf("监听 0.0.0.0:9000（FD_SETSIZE=%d，sizeof(fd_set)=%zu 字节）\n", FD_SETSIZE, sizeof(fd_set));
    fflush(stdout);

    // 自己维护的连接列表。rfds 只是每轮临时交给内核的"问题清单"，select 返回时会被改写，
    // 不能用它来记住有哪些连接
    static int clients[MAX_CLIENTS];
    int nclient = 0;
    char buf[1024];
    fd_set rfds;

    while (1) {
        // 1. 每轮都根据clients[]重新生成rfds，同时算出最大fd
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        int maxfd = sockfd;
        for (int i = 0; i < nclient; i++) {
            FD_SET(clients[i], &rfds); // fd >= FD_SETSIZE 时这里越界（任务 B）
            if (clients[i] > maxfd) {
                maxfd = clients[i];
            }
        }

        // 2. 睡在这里，直到至少一个fd可读。第一个参数是"最大fd + 1"，内核只检查 [0, maxfd] 这些位
        int n = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("select"); // 比如 EBADF：位图里放了已经关掉的 fd，说明代码有 bug
            exit(1);
        }

        // 3. 监听socket可读：有新连接
        if (FD_ISSET(sockfd, &rfds)) {
            struct sockaddr_in cli_addr;
            socklen_t cli_addr_len = sizeof(cli_addr);
            int clientfd = accept4(sockfd, (struct sockaddr *)&cli_addr, &cli_addr_len, SOCK_NONBLOCK);
            if (clientfd < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    perror("accept4"); // 比如 EMFILE：fd 用完了
                }
            } else if (nclient == MAX_CLIENTS) {
                fprintf(stderr, "连接数已满（%d），拒绝 fd=%d\n", MAX_CLIENTS, clientfd);
                close(clientfd);
            } else {
                clients[nclient++] = clientfd;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cli_addr.sin_addr, ip, sizeof(ip));
                // 连接多时只每 100 个打印一次，避免刷屏
                if (nclient <= 5 || nclient % 100 == 0) {
                    printf("客户端 %s:%u 已连接（fd=%d，当前 %d 个连接）\n", ip, ntohs(cli_addr.sin_port), clientfd, nclient);
                    fflush(stdout);
                }
            }
        }

        // 4. 处理就绪的客户端。每个就绪的fd只读一次，没读完的下一轮select还会报告。
        // 倒着遍历：删除时把最后一个挪到位置i，倒着走不会漏掉被挪过来的那个
        for (int i = nclient - 1; i >= 0; i--) {
            int fd = clients[i];
            if (!FD_ISSET(fd, &rfds)) {
                continue;
            }
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r > 0) {
                if (write_all(fd, buf, (size_t)r) == 0) {
                    continue;
                }
                perror("write");
            } else if (r == 0) {
                // 对方关闭，正常断开
            } else if (errno == EAGAIN || errno == EINTR) {
                continue; // select 说可读，但现在又没数据了，下一轮再说
            } else {
                perror("read");
            }
            // 走到这里：断开、写失败或读出错，关闭并从数组移除
            close(fd);
            clients[i] = clients[--nclient];
        }
    }
}
