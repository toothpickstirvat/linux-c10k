# 第 6 节：epoll水平触发（LT）

## 为什么学

epoll把“要监视哪些fd”和“等待事件”拆成两个调用：监视集合只在增删时告诉内核一次（`epoll_ctl`），内核在fd就绪时把它挂到一个就绪链表上，
`epoll_wait`只返回链表里的fd。1万个连接里只有1个活跃时，epoll_wait只处理1个，不再是O(n)。这是解决C10K的关键。

---

## 要学的内容

1. `epoll_create1(EPOLL_CLOEXEC)`创建epoll实例，它本身也是一个fd
2. `epoll_ctl(epfd, EPOLL_CTL_ADD/MOD/DEL, fd, &ev)`，`ev.data`可以放fd，也可以放指针（第9节放连接结构体）
3. `epoll_wait(epfd, events, maxevents, timeout)`返回就绪的个数
4. 水平触发（默认）：只要fd还可读（缓冲区里还有数据），每次epoll_wait都会返回它
5. 内核结构：红黑树存监视集合，就绪链表存就绪的fd，fd就绪时由回调挂上链表

---

## 任务

### 任务 A：epoll LT版echo服务器

`server_epoll_lt.c`：用法 `./server_epoll_lt [每次读取字节数] [v]`。listenfd和客户端fd都注册`EPOLLIN`，读一次后写回；关闭连接时先
`EPOLL_CTL_DEL`再`close`。

### 任务 B：验证“只返回就绪的”

建5000个空闲连接 + 1个活跃连接（`active.py`，20000次往返），和第5节poll版对比。

#### 实测结果

|          | 20000次往返用时 | 平均延迟 | active.py期间服务器消耗CPU | 空闲5秒消耗CPU |
|----------|-----------------|----------|----------------------------|----------------|
| epoll LT | 1.69 秒         | 84.5 µs  | 0.47 秒                    | 0              |
| poll     | 17.56 秒        | 878.1 µs | 17.41 秒                   | 0              |

- CPU时间读自`/proc/<pid>/stat`的utime+stime（1 tick = 10ms）。另一次独立测量是epoll 75.3 µs、poll 753.9 µs，数值有波动，但比例一致。
- **epoll的延迟和空闲连接数基本无关**，5000个空闲连接下和第4节几乎没有空闲连接时的select（86.7 µs）同一量级。
- **poll在5000个空闲连接下比epoll慢约10倍**，几乎把一个核吃满；poll在1000个空闲连接时是167.2 µs，5000个时约750到880
  µs，延迟随连接数线性上升。
- 两者空闲时CPU都是0，差距只出现在有活跃连接时：poll每次唤醒都要扫描全部5000项，epoll只处理就绪的那1个。

### 任务 C：LT会重复通知

`./server_epoll_lt 1 v`：每次只读1字节，客户端一次发10个字节。

实测：同一个fd被`epoll_wait`返回了**10次**，每次`read=1`；第11次是客户端关闭后的`read=0`。缓冲区里只要还有数据，LT每轮都会重复通知。

---

## 写之前先想清楚

1. `close(fd)`会自动把fd从epoll里删掉吗？什么情况下不会？（提示：fd被dup过、或者fork后子进程还开着）

   > **答：** 只有指向这个底层socket的最后一个fd被关闭时才会自动删除。epoll监视的是内核里打开的文件，不是fd号。
   > 实测（socketpair，注册fd后`dup`）：`close`原fd后，epoll仍然能收到事件；副本也关闭后才不再有事件。
   > fork后同样成立，已实测（socketpair，父进程close后子进程仍持有，epoll仍收到事件；子进程退出后事件才消失）。所以服务器关闭连接时，先显式`EPOLL_CTL_DEL`再`close`更稳妥。

2. 为什么LT下可写事件（`EPOLLOUT`）不能一直注册着？

   > **答（已实测）：** 实测（socketpair，LT 下一直注册 `EPOLLOUT`）：1 秒内 `epoll_wait` 返回 1190276 次，几乎没有一次真正在等；同样注册但改用 ET，1 秒内只收到 1 次事件。原因是发送缓冲区平时几乎都是空的，也就是“可写”。LT下只要可写，每次`epoll_wait`都会立刻返回，
   > 没数据要发时也一样，结果变成忙等，CPU被吃满。正确做法是只在`write`返回`EAGAIN`、有数据没写完时才注册`EPOLLOUT`
   ，写完立刻取消（下一节实现）。

3. epoll_wait的maxevents设成64，就绪的有1000个，剩下的会丢吗？

   > **答：** 不会丢。剩下的留在就绪链表里，下一次`epoll_wait`继续返回。
   > 实测（200个就绪fd，maxevents=64）：前4次累计见过64、128、192、200个不同fd，全部拿到。
   > 第5次仍返回64个，是因为本实验没人去读，LT下它们一直就绪。

---

## 自查方法

- 5000+ 连接正常回显，空闲时CPU为0。
- 能用数字说明epoll和poll在“大量空闲连接”场景下的差距：5000个空闲连接，延迟约84 µs对878 µs，CPU 0.47秒对17.41秒。

---

## 结论

1. **epoll的开销只和就绪连接数有关，和总连接数无关。** 监视集合只用`epoll_ctl`告诉内核一次，`epoll_wait`只返回就绪的。
2. **LT是默认模式，最安全：** 数据没读完下一轮还会通知，不会丢事件，代价是可能重复通知。
3. **不再需要自己维护连接数组：** 没有realloc，也没有挪元素和倒着遍历的问题。
4. **`maxevents`只是一次取多少，不会丢就绪事件。**

## 踩坑

1. **只检查`EPOLLIN`：** `EPOLLHUP`/`EPOLLERR`不用申请也会报告，只要`events`非0就交给`read`判断。
2. **关闭连接不`EPOLL_CTL_DEL`：** fd被dup或fork后别处还开着时，close不会把它从epoll里删掉。
3. **LT下一直注册`EPOLLOUT`：** 忙等，CPU满。
4. **测试脚本里把`cd && ulimit && ./server &`连成一串放后台：** `$!`记下的是子shell的PID，`kill`
   杀不到真正的服务器，旧服务器一直占着9000端口，新服务器`bind`失败，测到的是旧服务器。要把启动和`SP=$!`分成独立的行，并检查
   `kill -0 $SP`。
5. **`pkill -f 脚本名`：** 会匹配到执行命令的shell自己，把自己杀掉。应按进程名精确匹配。
6. **EMFILE：** accept失败时连接留在队列里，listenfd一直可读，会空转（见第4节）。

## 最佳实践

1. listenfd和connfd都设成非阻塞，用`accept4(SOCK_NONBLOCK)`。
2. 关闭连接：先`EPOLL_CTL_DEL`，再`close`。
3. `epoll_ctl`、`epoll_create1`的返回值都要检查。
4. `perf`受限时，可以读`/proc/<pid>/stat`的utime+stime来量CPU时间。
5. 只在需要时才注册`EPOLLOUT`。
