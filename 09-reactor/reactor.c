// 第 9 节：Reactor
//
// 用法：./reactor
//   任务 A：单线程事件循环，按行 echo，带读写缓冲、写回压和 30 秒空闲超时
//   任务 B：后面再加线程数参数，每个线程一个 epoll + SO_REUSEPORT
//
// 设计要点：
//   1. 每个连接一个 conn_t，epoll 的 ev.data.ptr 指向它
//   2. 按行分帧：rbuf 里找到 '\n' 才算一条完整消息
//   3. 写回压：write 写不完的部分放进 wbuf 并注册 EPOLLOUT，写空后立刻取消；wbuf 有上限，超过就关连接
//   4. 超时：timerfd 每秒触发一次，扫描并关闭 30 秒没收到数据的连接
//   5. use-after-free：关闭的连接先放进待释放列表，一轮事件处理完再统一 free
//
// 本文件分块实现，各区块在下面用“区块 N”标出。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>    // pthread_create, pthread_detach（任务 B 多线程）
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9000
#define MAX_EVENTS 256
#define RBUF_SIZE 4096            // 读缓冲大小，一行最长不能超过它
#define WBUF_LIMIT (1024 * 1024)  // 写缓冲上限 1 MB，超过就关连接
#define IDLE_TIMEOUT 30           // 空闲超时秒数

// 一个连接的状态，ev.data.ptr 指向它
typedef struct conn {
    int fd;
    char rbuf[RBUF_SIZE];
    size_t rlen;        // rbuf 里已有的字节数
    char *wbuf;         // 写缓冲：write 没写完的部分
    size_t wlen, wcap;
    int want_out;       // 当前是否注册了 EPOLLOUT
    int closed;         // 已关闭，等待统一释放
    struct conn *prev, *next; // 所有连接的双向链表：超时扫描要遍历，关闭时 O(1) 摘除
    time_t last_active; // 最后一次收到数据的时间
} conn_t;

// ===== 区块 1：连接表与待释放列表 =====
// 任务 B：每个线程一个独立的 Reactor，这些全局状态都改成线程私有（__thread），线程之间不共享连接，所以不用加锁
static __thread int epfd = -1;
static __thread conn_t *conn_head = NULL; // 本线程所有存活连接的链表头
static __thread conn_t *pending = NULL;   // 待释放列表（复用 next 指针）
static __thread long n_conn = 0;          // 本线程当前存活连接数

// 把新连接挂到存活链表头部
static void conn_link(conn_t *c) {
    c->prev = NULL;
    c->next = conn_head;
    if (conn_head != NULL) {
        conn_head->prev = c;
    }
    conn_head = c;
    n_conn++;
}

// 从存活链表摘除，O(1)
static void conn_unlink(conn_t *c) {
    if (c->prev != NULL) {
        c->prev->next = c->next;
    } else {
        conn_head = c->next;
    }
    if (c->next != NULL) {
        c->next->prev = c->prev;
    }
    c->prev = NULL;
    c->next = NULL;
    n_conn--;
}

// 关闭连接：先 DEL 再 close，摘出存活链表，但不立刻 free。
// 同一轮 epoll_wait 返回的后续事件可能还指着它，所以放进待释放列表，一轮结束后统一 free；
// closed 标志让后面的事件处理能跳过它，重复调用也安全
static void conn_close(conn_t *c) {
    if (c->closed) {
        return;
    }
    c->closed = 1;
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_unlink(c);
    c->next = pending;
    pending = c;
}

// 一轮事件处理完后调用，真正释放已关闭的连接
static void free_pending(void) {
    while (pending != NULL) {
        conn_t *n = pending->next;
        free(pending->wbuf);
        free(pending);
        pending = n;
    }
}

// ===== 区块 2：读、分帧与回显 =====
static void conn_send(conn_t *c, const char *data, size_t len); // 区块 3：带写回压的发送

// EPOLLIN 触发：LT 模式下每次通知只 read 一次，没读完下一轮还会通知，不会饿死别的连接。
// TCP 是字节流，一次 read 可能是半行，也可能有好几行：追加到 rbuf，找到 '\n' 才算一条完整消息
static void on_readable(conn_t *c) {
    ssize_t r = read(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen);
    if (r == 0) {
        conn_close(c); // 对方关闭
        return;
    }
    if (r < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            conn_close(c); // ECONNRESET 等
        }
        return;
    }
    c->rlen += (size_t)r;
    c->last_active = time(NULL);

    // 循环取出缓冲区里所有完整的行，原样回显
    size_t start = 0;
    while (start < c->rlen) {
        char *nl = memchr(c->rbuf + start, '\n', c->rlen - start);
        if (nl == NULL) {
            break;
        }
        size_t linelen = (size_t)(nl - (c->rbuf + start)) + 1;
        conn_send(c, c->rbuf + start, linelen);
        if (c->closed) {
            return; // 发送时写缓冲超限被关掉了，不要再碰这个连接
        }
        start += linelen;
    }
    // 把没凑成一行的残余数据挪到缓冲区开头
    memmove(c->rbuf, c->rbuf + start, c->rlen - start);
    c->rlen -= start;

    // 缓冲区满了还没有换行：这一行超过 RBUF_SIZE，没法再接收，关掉连接
    if (c->rlen == RBUF_SIZE) {
        fprintf(stderr, "fd=%d 的一行超过 %d 字节，关闭连接\n", c->fd, RBUF_SIZE);
        conn_close(c);
    }
}

// ===== 区块 3：写回压与 EPOLLOUT =====
// 开关 EPOLLOUT。LT 下 EPOLLOUT 只要发送缓冲区有空位就一直触发，所以只在 wbuf 里有数据时才注册，写空立刻取消
static void set_epollout(conn_t *c, int on) {
    if (c->want_out == on) {
        return;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | (on ? EPOLLOUT : 0);
    ev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
        perror("epoll_ctl MOD");
        conn_close(c);
        return;
    }
    c->want_out = on;
}

// 把数据追加到 wbuf，容量翻倍增长。超过 WBUF_LIMIT 返回 -1：对方一直不读时不能让内存无限增长
static int wbuf_append(conn_t *c, const char *data, size_t len) {
    if (c->wlen + len > WBUF_LIMIT) {
        return -1;
    }
    if (c->wlen + len > c->wcap) {
        size_t ncap = c->wcap ? c->wcap : 4096;
        while (ncap < c->wlen + len) {
            ncap *= 2;
        }
        char *p = realloc(c->wbuf, ncap); // 用临时变量接，失败时原指针仍然有效
        if (p == NULL) {
            return -1;
        }
        c->wbuf = p;
        c->wcap = ncap;
    }
    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;
    return 0;
}

// 带写回压的发送：wbuf 里已有数据时只能排在后面（保证顺序）；否则先直接 write，写不完的部分进 wbuf
static void conn_send(conn_t *c, const char *data, size_t len) {
    size_t off = 0;
    if (c->wlen == 0) {
        while (off < len) {
            ssize_t n = write(c->fd, data + off, len - off);
            if (n > 0) {
                off += (size_t)n;
                continue;
            }
            if (n == -1 && errno == EINTR) {
                continue;
            }
            if (n == -1 && errno == EAGAIN) {
                break; // 发送缓冲区满了，剩下的存起来
            }
            conn_close(c); // EPIPE、ECONNRESET 等
            return;
        }
        if (off == len) {
            return;
        }
    }
    if (wbuf_append(c, data + off, len - off) == -1) {
        fprintf(stderr, "fd=%d 写缓冲超过 %d 字节，关闭连接\n", c->fd, WBUF_LIMIT);
        conn_close(c);
        return;
    }
    set_epollout(c, 1);
}

// EPOLLOUT 触发：继续把 wbuf 写出去，写空了取消 EPOLLOUT
static void on_writable(conn_t *c) {
    size_t off = 0;
    while (off < c->wlen) {
        ssize_t n = write(c->fd, c->wbuf + off, c->wlen - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n == -1 && errno == EAGAIN) {
            break;
        }
        conn_close(c);
        return;
    }
    memmove(c->wbuf, c->wbuf + off, c->wlen - off);
    c->wlen -= off;
    if (c->wlen == 0) {
        set_epollout(c, 0);
    }
}

// ===== 区块 4：timerfd 空闲超时 =====
// ev.data.ptr 对连接指向 conn_t；listenfd 和 timerfd 没有 conn_t，用这两个静态变量的地址当标记，
// 事件循环里比较指针就能区分三种事件
static char tag_listen;
static char tag_timer;
static __thread int timerfd = -1; // 每个线程自己的定时器 fd

// 创建每秒触发一次的 timerfd 并加入 epoll。定时器是 fd，和 socket 一起交给同一个 epoll，不需要信号
static int timer_setup(void) {
    timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) {
        return -1;
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 1;    // 首次触发
    its.it_interval.tv_sec = 1; // 之后每秒一次
    if (timerfd_settime(timerfd, 0, &its, NULL) == -1) {
        return -1;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &tag_timer;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, timerfd, &ev);
}

// timerfd 可读：必须 read 走 8 字节的到期次数，否则 LT 下会一直通知。
// 然后扫描所有连接，关闭空闲超过 IDLE_TIMEOUT 秒的。扫描是 O(n)，最简单；时间轮或最小堆更高效
static void on_timer(void) {
    uint64_t expirations;
    if (read(timerfd, &expirations, sizeof(expirations)) == -1 && errno != EAGAIN) {
        perror("read timerfd");
    }
    time_t now = time(NULL);
    // conn_close 会改写 c->next（挂进待释放列表），所以遍历前先保存下一个
    for (conn_t *c = conn_head, *nx; c != NULL; c = nx) {
        nx = c->next;
        if (now - c->last_active > IDLE_TIMEOUT) {
            fprintf(stderr, "fd=%d 空闲超过 %d 秒，关闭连接\n", c->fd, IDLE_TIMEOUT);
            conn_close(c);
        }
    }
}

// ===== 区块 5：监听 socket 与 accept =====
static __thread int listenfd = -1; // 每个线程自己的监听 socket（SO_REUSEPORT 让各线程绑定同一个端口）

// 创建非阻塞的监听 socket 并加入 epoll。
// SO_REUSEPORT 必须在 bind 之前设置：任务 B 里每个线程各自监听同一个端口，由内核把新连接分给各线程；单线程时没有副作用
static int listen_setup(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return -1;
    }
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == -1) {
        close(fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 || listen(fd, SOMAXCONN) == -1) {
        close(fd);
        return -1;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &tag_listen;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        close(fd);
        return -1;
    }
    listenfd = fd;
    return 0;
}

// listenfd 可读：循环 accept 到 EAGAIN，一次把队列取空
static void on_accept(void) {
    while (1) {
        int fd = accept4(listenfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept4"); // 比如 EMFILE：fd 用完了，连接留在队列里（见第 4 节）
            break;
        }
        conn_t *c = calloc(1, sizeof(*c)); // 清零：rlen、wlen、want_out、closed 都从 0 开始
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->last_active = time(NULL);
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl ADD");
            free(c);
            close(fd);
            continue;
        }
        conn_link(c);
    }
}

// 一个完整的 Reactor：创建本线程自己的epoll、listenfd、timerfd，然后进入事件循环，正常情况下不会返回
static int reactor_run(void) {
    // ===== 区块 6：创建 listenfd、epoll、timerfd 并进入事件循环 =====
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
        return 1;
    }
    if (listen_setup() == -1) {
        perror("listen_setup");
        return 1;
    }
    if (timer_setup() == -1) {
        perror("timer_setup");
        return 1;
    }
    printf("reactor 监听 0.0.0.0:%d（单线程，空闲超时 %d 秒，写缓冲上限 %d 字节）\n", PORT, IDLE_TIMEOUT, WBUF_LIMIT);
    fflush(stdout);

    struct epoll_event events[MAX_EVENTS];
    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            return 1;
        }
        for (int i = 0; i < n; i++) {
            void *p = events[i].data.ptr;
            if (p == &tag_listen) {
                on_accept();
                continue;
            }
            if (p == &tag_timer) {
                on_timer();
                continue;
            }
            conn_t *c = p;
            if (c->closed) {
                continue; // 本轮前面的事件已经把它关了，内存还没释放，跳过
            }
            uint32_t ev = events[i].events;
            // EPOLLERR/EPOLLHUP 不用申请也会报告：有读事件就交给read判断，否则直接关闭
            if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                on_readable(c);
                if (c->closed) {
                    continue;
                }
            }
            if (ev & EPOLLOUT) {
                on_writable(c);
            }
        }
        // 一轮事件处理完，统一释放已关闭的连接，后面的事件不会再碰到悬空指针
        free_pending();
    }
}

// 线程入口：每个线程跑一个独立的Reactor（自己的epoll、listenfd、timerfd、连接链表）
static void *thread_main(void *arg) {
    (void)arg;
    reactor_run();
    return NULL;
}

// 用法：./reactor [线程数]，默认1。线程数大于1时，每个线程各自bind同一个端口（SO_REUSEPORT），
// 内核按四元组哈希把新连接分给各个线程的listenfd，线程之间没有共享的连接，不需要加锁
int main(int argc, char *argv[]) {
    long nthreads = 1;
    if (argc >= 2) {
        char *end;
        nthreads = strtol(argv[1], &end, 10);
        if (*end != '\0' || nthreads < 1 || nthreads > 64) {
            fprintf(stderr, "用法：%s [线程数 1-64]\n", argv[0]);
            return 1;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    // 主线程自己也跑一个Reactor，所以只需要再创建nthreads - 1个线程
    for (long i = 1; i < nthreads; i++) {
        pthread_t tid;
        int err = pthread_create(&tid, NULL, thread_main, NULL);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err)); // pthread 函数返回错误码，不设置 errno
            return 1;
        }
        pthread_detach(tid);
    }
    return reactor_run();
}
