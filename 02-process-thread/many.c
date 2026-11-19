// 第 2 节 任务 C：一次建立 N 个连接并一直保持，用来测量服务器每个连接的开销
//
// 用法：./many <ip> <port> <N>
// 例：  ./many 127.0.0.1 9000 1000
//
// 全部连上后调用 pause() 挂起，按 Ctrl+C 退出，进程退出时内核会关闭所有 fd。
// 注意：每个连接占客户端一个 fd，N 不能超过 ulimit -n；
// 连同一个服务器地址时，客户端的源端口来自 ip_local_port_range（本机 32768–60999，约 2.8 万个）。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>     // close, pause
#include <sys/socket.h> // socket, connect
#include <netinet/in.h> // struct sockaddr_in, htons
#include <arpa/inet.h>  // inet_pton

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "用法：%s <ip> <port> <N>\n", argv[0]);
        return 1;
    }

    // 用 strtol 而不是 atoi：atoi 遇到非法输入直接返回 0，分不出是"输入了 0"还是"输入错了"
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (*end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "端口不合法：%s\n", argv[2]);
        return 1;
    }
    long n = strtol(argv[3], &end, 10);
    if (*end != '\0' || n <= 0) {
        fprintf(stderr, "连接数不合法：%s\n", argv[3]);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    // inet_pton 返回 1 才是成功；0 表示地址格式不对（这时不设置 errno，不能用 perror）
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "IP 地址不合法：%s\n", argv[1]);
        return 1;
    }

    long ok = 0;
    for (long i = 0; i < n; i++) {
        // 客户端不需要 bind：connect 时内核自动选一个本地端口
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
            // 最常见的是 EMFILE：fd 用完了，超过了 ulimit -n
            perror("socket");
            break;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            // ECONNREFUSED：服务器没启动；EADDRNOTAVAIL：本地端口用完了
            perror("connect");
            close(fd);
            break;
        }
        ok++; // fd 故意不关，也不用保存：pause 期间连接一直保持，进程退出时内核统一关闭
        if (ok % 1000 == 0) {
            printf("已建立 %ld 个连接\n", ok);
        }
    }

    printf("共建立 %ld 个连接（目标 %ld），按 Ctrl+C 退出\n", ok, n);
    fflush(stdout);
    pause(); // 挂起，直到收到信号
    return 0;
}
