// 第 2 节 任务 B：一个连接一个线程（pthread 版）
//
// 主线程只负责 accept，每来一个连接就创建一个线程跑 echo 循环。
// 和 fork 版的区别：所有线程共用一张 fd 表，所以主线程不能关 connfd，由处理线程自己关；
// 线程用 pthread_detach 让系统自动回收，不需要 SIGCHLD + waitpid。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, SIGPIPE
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

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

// 传给处理线程的参数：每个连接 malloc 一份，由处理线程负责 free。
// 不能传 &connfd 或 &pclient：它们是主线程栈上的同一个变量，下一轮 accept 就会被覆盖。
typedef struct client_t
{
    int connfd;
    char ip[INET_ADDRSTRLEN];
    unsigned port;
} client_t;

static void* handle_conn(void *arg) {
    client_t *pclient = arg; // 直接就是 malloc 出来的那块内存
    char buf[1024];
    while (1) {
        ssize_t n = read(pclient->connfd, buf, sizeof(buf));
        if (n > 0) {
            printf("<-- %s:%u: %.*s", pclient->ip, pclient->port, (int)n, buf);
            if (write_all(pclient->connfd, buf, (size_t)n) == -1) {
                perror("write");
                break;
            }
            continue;
        }
        if (n == 0) { // 客户端断开
            printf("客户端 %s:%u 断开\n", pclient->ip, pclient->port);
            break;
        }
        // n == -1
        if (errno == EINTR) { // 被信号打断，重试
            continue;
        }
        perror("read"); // 比如 ECONNRESET
        break;
    }
    // 三种退出情况都走到这里，统一清理。fd 只在这一个地方关。
    close(pclient->connfd);
    free(pclient);
    return NULL;
}

int main(void) {
    // 对方关闭后再 write 会收到 SIGPIPE，默认动作是杀死整个进程。
    // 信号是发给进程的，线程版里一个连接触发 SIGPIPE，所有连接都会一起断掉，所以必须忽略。
    signal(SIGPIPE, SIG_IGN);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }

    int one = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        exit(1);
    }

    if (listen(sockfd, SOMAXCONN) == -1) {
        perror("listen");
        exit(1);
    }
    printf("监听 0.0.0.0:9000\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int connfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (connfd < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        unsigned port = ntohs(client_addr.sin_port);
        printf("客户端 %s:%u 已连接（fd=%d）\n", ip, port, connfd);

        client_t *pclient = malloc(sizeof(client_t));
        if (pclient == NULL) {
            perror("malloc");
            close(connfd);
            continue;
        }
        pclient->connfd = connfd;
        pclient->port = port;
        memcpy(pclient->ip, ip, sizeof(ip));

        pthread_t tid;
        int err = pthread_create(&tid, NULL, handle_conn, pclient);
        if (err != 0) {
            // pthread 系列函数不设置 errno，错误码就是返回值
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            close(connfd); // 线程没创建出来，fd 和参数只能由主线程清理
            free(pclient);
            continue;
        }
        // 分离线程：线程结束后栈和描述符由系统自动回收，不需要 join（否则会像僵尸进程一样留着）
        pthread_detach(tid);
    }

    close(sockfd);

    return 0;
}
