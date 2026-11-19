// 第 2 节 任务A：一个连接一个进程（fork版）
//
// 父进程只负责accept，每来一个连接就fork一个子进程去跑echo循环。
// 子进程退出时内核给父进程发SIGCHLD，在处理函数里用waitpid回收，避免留下僵尸进程
// （不回收的话，ps里会看到 <defunct>）。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>     // signal, sigaction, SIGPIPE, SIGCHLD
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>   // waitpid, WNOHANG

// SIGCHLD 处理函数：回收所有已经退出的子进程。
// 1. 必须循环：同一种信号在等待处理期间不会排队，5 个子进程同时退出，可能只收到 1 次 SIGCHLD，
//    只调一次 waitpid 就会漏掉其余的，留下僵尸。
// 2. 条件必须是 > 0：返回 0 表示还有子进程但都没退出；返回 -1（ECHILD）表示已经没有子进程了。
//    写成 while (waitpid(...)) 的话 -1 也为真，最后一个子进程回收后会在这里死循环。
// 3. 保存并恢复 errno：信号可能在主流程刚拿到 -1、还没检查 errno 时到来，
//    waitpid 会改写 errno，不恢复的话主流程看到的就是错的错误码。
// 4. 处理函数里只能调用异步信号安全的函数（man 7 signal-safety）：waitpid 可以，printf 不行。
static void on_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // 每次回收一个，直到没有已退出的子进程
    }
    errno = saved_errno;
}

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
    // 对方关闭后再 write 会收到 SIGPIPE，默认直接杀死进程。
    // 信号处理方式会被 fork 出来的子进程继承，所以在这里设置一次就够了。
    signal(SIGPIPE, SIG_IGN);

    // 注册 SIGCHLD 处理函数。用 sigaction 而不是 signal：signal 的语义在不同系统上不一致。
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    // sa_flags = 0：信号打断阻塞的 accept 时，accept 返回 -1、errno = EINTR，下面要自己处理。
    // 改成 SA_RESTART 的话，内核会自动重新执行被打断的 accept，就看不到 EINTR 了。
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

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
            if (errno == EINTR) {
                // 子进程退出，SIGCHLD打断了阻塞的accept。不是错误，重新accept即可。
                // 这一行只是为了观察现象；改成SA_RESTART后就不会再打印了。
                printf("accept 被信号打断（EINTR），重试\n");
                continue;
            }
            perror("accept");
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        unsigned port = ntohs(client_addr.sin_port);
        printf("客户端 %s:%u 已连接（fd=%d）\n", ip, port, connfd);

        // fork 会把stdio缓冲区也复制一份。stdout重定向到文件时是全缓冲，
        // 不先刷新的话，父进程还没写出去的日志会被每个子进程在exit时重复写一遍。
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(connfd);
            continue;
        }
        if (pid == 0) { // 子进程：只处理这一个连接，用不到监听socket
            close(sockfd);
            char buf[1024];
            while (1) {
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
                if (n == 0) { // 对方关闭了连接（收到FIN）
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
            close(connfd);
            // 必须退出，否则会继续往下走，回到外层循环去 accept（而 sockfd 已经关了）
            exit(0);
        }

        // 父进程：连接交给子进程了，父进程这份 connfd 要关掉，
        // 否则客户端断开、子进程退出后，这个连接还被父进程引用着，无法真正关闭
        close(connfd);
    }
    return 0;
}
