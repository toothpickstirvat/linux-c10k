// 第 8 节 任务 A：C10K 压测客户端 loadgen
//
// 用法：./loadgen <ip> <port> <连接数N> <每连接每秒请求数> <持续秒数>
// 例：  ./loadgen 127.0.0.1 9000 10000 1 10
//
// 单线程 + epoll，客户端自己不能成为瓶颈：
//   1. 非阻塞 connect：EINPROGRESS -> 等 EPOLLOUT -> getsockopt(SO_ERROR) 确认
//   2. 每个连接按频率发一条 16 字节消息（前 8 字节是发送时间戳，单位纳秒），收满 16 字节回显后算延迟
//   3. 结束时打印建连成功数、建连耗时、总请求数、p50/p99 延迟
//
// 本文件分块实现，各区块在下面用“区块 N”标出。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_EVENTS 256
#define MSG_LEN 16 // 每条消息的长度：前 8 字节放发送时间戳，其余补零

// 一个连接的状态，epoll 的 ev.data.ptr 指向它
typedef struct {
    int fd;
    int connected;       // 0 = 正在建连，1 = 已连上
    char rbuf[MSG_LEN];  // 回显可能分几次到达，攒满 MSG_LEN 才算一次完整回显
    int rlen;            // rbuf 里已有的字节数
    uint64_t next_send;  // 下一次发送的时间（纳秒，单调时钟）
} conn_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// ===== 区块 1：全局状态与延迟统计 =====
static uint64_t *lat;    // 所有延迟样本（纳秒），结束时排序取 p50/p99
static size_t nlat = 0;  // 已记录的样本数
static size_t lat_cap = 0;

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

// 记录一个延迟样本，满了就容量翻倍。realloc 用临时变量接返回值，失败时原数组仍然有效
static int lat_add(uint64_t v) {
    if (nlat == lat_cap) {
        size_t ncap = lat_cap ? lat_cap * 2 : 65536;
        uint64_t *p = realloc(lat, ncap * sizeof(*p));
        if (p == NULL) {
            return -1;
        }
        lat = p;
        lat_cap = ncap;
    }
    lat[nlat++] = v;
    return 0;
}

// ===== 区块 2：建连 =====
static int epfd = -1;
static long n_ok = 0;         // 建连成功数
static long n_fail = 0;       // 建连失败数
static long n_req = 0;        // 收到完整回显的次数
static uint64_t interval_ns;  // 每个连接两次发送的间隔
static uint64_t t_conn_last;  // 最后一个连接建成的时刻，用来算建连耗时

// 从 epoll 里删掉并关闭。显式 DEL 再 close，和第 6 节一样
static void conn_close(conn_t *c) {
    if (c->fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
    }
}

// 连上之后：改成监听 EPOLLIN，并把首次发送时间在一个间隔内随机错开，避免所有连接同一瞬间发包
static void on_connected(conn_t *c, uint64_t now) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
        n_fail++;
        conn_close(c);
        return;
    }
    c->connected = 1;
    c->rlen = 0;
    n_ok++;
    t_conn_last = now;
    c->next_send = now + ((uint64_t)rand() * interval_ns) / ((uint64_t)RAND_MAX + 1);
}

// 发起一个非阻塞连接。connect 返回 -1 且 errno == EINPROGRESS 表示握手进行中，不是错误
static void start_connect(conn_t *c, const struct sockaddr_in *addr) {
    c->connected = 0;
    c->rlen = 0;
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (c->fd == -1) {
        perror("socket"); // 最常见的是 EMFILE：超过 ulimit -n
        n_fail++;
        return;
    }
    int rc = connect(c->fd, (const struct sockaddr *)addr, sizeof(*addr));
    if (rc == -1 && errno != EINPROGRESS) {
        n_fail++;
        close(c->fd);
        c->fd = -1;
        return;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT; // 可写表示握手结束，但成功还是失败要看 SO_ERROR
    ev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev) == -1) {
        n_fail++;
        close(c->fd);
        c->fd = -1;
        return;
    }
    if (rc == 0) {
        on_connected(c, now_ns()); // 本机回环偶尔会立刻连上
    }
}

// EPOLLOUT 触发：用 getsockopt(SO_ERROR) 确认握手是否真的成功
static void on_writable(conn_t *c) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1 || err != 0) {
        n_fail++;
        conn_close(c);
        return;
    }
    on_connected(c, now_ns());
}

// ===== 区块 3：发送与接收回显 =====
static long n_drop = 0; // 连上之后又出错或被对方关闭的连接数

// 发送一条消息：前 8 字节是当前时间戳，其余补零。
// 16 字节远小于发送缓冲区，正常不会写不完；写不完会破坏消息边界，按连接出错处理
static void send_one(conn_t *c, uint64_t now) {
    // 文本格式：15 位十六进制时间戳 + '\n'，正好 MSG_LEN 字节。以换行结尾，按行分帧的服务器（第 9 节 reactor）才会回显
    char msg[MSG_LEN + 1]; // snprintf 会多写一个结尾的 '\0'
    snprintf(msg, sizeof(msg), "%015llx\n", (unsigned long long)now);
    ssize_t n = write(c->fd, msg, MSG_LEN);
    if (n == (ssize_t)MSG_LEN) {
        return;
    }
    if (n == -1 && (errno == EAGAIN || errno == EINTR)) {
        return; // 这次发不出去就跳过，下个间隔再发
    }
    n_drop++;
    conn_close(c);
}

// EPOLLIN 触发：读回显。TCP 是字节流，rbuf 攒满 MSG_LEN 才算一次完整往返。
// 每次最多读到凑满一条消息，多出来的留在内核里，LT 下下一轮还会通知
static void on_readable(conn_t *c) {
    ssize_t r = read(c->fd, c->rbuf + c->rlen, MSG_LEN - c->rlen);
    if (r > 0) {
        c->rlen += (int)r;
        if (c->rlen == MSG_LEN) {
            // 消息是 15 位十六进制文本 + '\n'，要按文本解析回发送时间戳
            char tmp[MSG_LEN];
            memcpy(tmp, c->rbuf, MSG_LEN - 1);
            tmp[MSG_LEN - 1] = '\0';
            uint64_t sent = strtoull(tmp, NULL, 16);
            uint64_t now = now_ns();
            if (now >= sent) {
                if (lat_add(now - sent) == -1) {
                    fprintf(stderr, "realloc 失败，丢弃延迟样本\n");
                }
            }
            n_req++;
            c->rlen = 0;
        }
        return;
    }
    if (r == -1 && (errno == EAGAIN || errno == EINTR)) {
        return;
    }
    // r == 0：服务器关闭了连接；r == -1：ECONNRESET 等真正的错误
    n_drop++;
    conn_close(c);
}

// ===== 区块 4：统计输出 =====
static void print_stats(long nconn, long duration, uint64_t t_start) {
    printf("目标连接数 %ld，建连成功 %ld，建连失败 %ld，连上后中断 %ld\n", nconn, n_ok, n_fail, n_drop);
    if (n_ok > 0 && t_conn_last >= t_start) {
        printf("建连耗时 %.1f 毫秒（从开始到最后一个连上）\n", (double)(t_conn_last - t_start) / 1e6);
    }
    printf("持续 %ld 秒，收到完整回显 %ld 次（约 %.0f 次/秒）\n", duration, n_req, (double)n_req / (double)duration);
    if (nlat == 0) {
        printf("没有延迟样本\n");
        return;
    }
    // 排序后取分位数：p50 是中间那个，p99 表示 99% 的请求不比它慢
    qsort(lat, nlat, sizeof(*lat), cmp_u64);
    size_t i50 = nlat * 50 / 100; // 下标先算好，避免整数除法出现在浮点表达式里
    size_t i99 = nlat * 99 / 100;
    printf("延迟：p50 = %.1f 微秒，p99 = %.1f 微秒，最大 = %.1f 微秒（样本 %zu 个）\n",
           (double)lat[i50] / 1e3, (double)lat[i99] / 1e3, (double)lat[nlat - 1] / 1e3, nlat);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "用法：%s <ip> <port> <连接数N> <每连接每秒请求数> <持续秒数>\n", argv[0]);
        return 1;
    }

    // 用 strtol 而不是 atoi：atoi 分不出“输入了 0”和“输入错了”
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (*end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "端口不合法：%s\n", argv[2]);
        return 1;
    }
    long nconn = strtol(argv[3], &end, 10);
    if (*end != '\0' || nconn <= 0) {
        fprintf(stderr, "连接数不合法：%s\n", argv[3]);
        return 1;
    }
    long rate = strtol(argv[4], &end, 10);
    if (*end != '\0' || rate <= 0) {
        fprintf(stderr, "每连接每秒请求数不合法：%s\n", argv[4]);
        return 1;
    }
    long duration = strtol(argv[5], &end, 10);
    if (*end != '\0' || duration <= 0) {
        fprintf(stderr, "持续秒数不合法：%s\n", argv[5]);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "IP 地址不合法：%s\n", argv[1]);
        return 1;
    }

    // ===== 区块 5：主循环 =====
    interval_ns = 1000000000ULL / (uint64_t)rate;
    srand(12345);
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
        return 1;
    }
    conn_t *conns = calloc((size_t)nconn, sizeof(conn_t));
    if (conns == NULL) {
        perror("calloc");
        return 1;
    }
    for (long i = 0; i < nconn; i++) {
        conns[i].fd = -1;
    }

    // 第一阶段：一口气发起所有非阻塞 connect；握手结果在主循环里通过 EPOLLOUT 收集
    uint64_t t_start = now_ns();
    for (long i = 0; i < nconn; i++) {
        start_connect(&conns[i], &addr);
    }

    // 第二阶段：所有连接都有结果（成功或失败）后才开始计时发请求，建连最多等 30 秒
    uint64_t t_end = 0;
    uint64_t last_scan = 0;
    struct epoll_event events[MAX_EVENTS];
    while (1) {
        uint64_t now = now_ns();
        if (t_end == 0 && (n_ok + n_fail >= nconn || now - t_start > 30ULL * 1000000000ULL)) {
            t_end = now + (uint64_t)duration * 1000000000ULL;
        }
        if (t_end != 0 && now >= t_end) {
            break;
        }

        int n = epoll_wait(epfd, events, MAX_EVENTS, 5);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            conn_t *c = events[i].data.ptr;
            if (c->fd < 0) {
                continue;
            }
            if (!c->connected) {
                on_writable(c); // 握手结束（成功或失败），由 SO_ERROR 判断
            } else {
                on_readable(c);
            }
        }

        // 每 5 毫秒扫描一遍，给到了发送时间的连接发一条消息
        now = now_ns();
        if (t_end != 0 && now - last_scan >= 5000000ULL) {
            last_scan = now;
            for (long i = 0; i < nconn; i++) {
                conn_t *c = &conns[i];
                if (c->connected && c->fd >= 0 && now >= c->next_send) {
                    send_one(c, now);
                    c->next_send += interval_ns;
                    if (c->next_send < now) {
                        c->next_send = now + interval_ns; // 落后太多就不补发，避免突发
                    }
                }
            }
        }
    }

    print_stats(nconn, duration, t_start);
    free(conns);
    free(lat);
    return 0;
}
