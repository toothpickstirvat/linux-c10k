# 第 6 节：epoll 水平触发（LT）

## 为什么学

epoll 把"要监视哪些 fd"和"等待事件"拆成两个调用：监视集合只在增删时告诉内核一次（`epoll_ctl`），内核在 fd 就绪时把它挂到一个就绪链表上，
`epoll_wait` 只返回链表里的 fd。1 万个连接里只有 1 个活跃时，epoll_wait 只处理 1 个，不再是 O(n)。这是解决 C10K 的关键。

---

## 要学的内容

1. `epoll_create1(EPOLL_CLOEXEC)` 创建 epoll 实例，它本身也是一个 fd
2. `epoll_ctl(epfd, EPOLL_CTL_ADD/MOD/DEL, fd, &ev)`，`ev.data` 可以放 fd，也可以放指针（第 9 节放连接结构体）
3. `epoll_wait(epfd, events, maxevents, timeout)` 返回就绪的个数
4. 水平触发（默认）：只要 fd 还可读（缓冲区里还有数据），每次 epoll_wait 都会返回它
5. 内核结构：红黑树存监视集合，就绪链表存就绪的 fd，fd 就绪时由回调挂上链表

---

## 任务

### 任务 A：epoll LT 版 echo 服务器

`server_epoll_lt.c`：listenfd 和客户端 fd 都注册 `EPOLLIN`。读一次（不必读完），写回。关闭连接前要不要 `EPOLL_CTL_DEL`？

### 任务 B：验证"只返回就绪的"

建 5000 个空闲连接 + 1 个活跃连接，打印每次 `epoll_wait` 的返回值；和第 5 节 poll 版对比 CPU（`perf stat -e task-clock`）。

### 任务 C：LT 会重复通知

每次只读 1 个字节，客户端一次发 10 个字节，数一数 epoll_wait 返回了几次这个 fd。

---

## 写之前先想清楚

1. `close(fd)` 会自动把 fd 从 epoll 里删掉吗？什么情况下不会？（提示：fd 被 dup 过、或者 fork 后子进程还开着）
2. 为什么 LT 下可写事件（`EPOLLOUT`）不能一直注册着？
3. epoll_wait 的 maxevents 设成 64，就绪的有 1000 个，剩下的会丢吗？

---

## 自查方法

- 5000+ 连接正常回显，空闲时 CPU 接近 0。
- 能用数字说明 epoll 和 poll 在"大量空闲连接"场景下的差距。
