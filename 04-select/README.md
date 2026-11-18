# 第 4 节：select

## 为什么学

select 是最早的 I/O 多路复用：把一组 fd 交给内核，内核在任意一个就绪（可读/可写/异常）或超时时返回。
线程阻塞在 select 上，不占 CPU，解决了第 3 节的空转问题。它的几个限制，正是 poll 和 epoll 出现的原因。

---

## 要学的内容

1. `fd_set` 是一个位图，`FD_ZERO`/`FD_SET`/`FD_ISSET`/`FD_CLR`
2. `select(nfds, &rfds, &wfds, &efds, &timeout)`：nfds 是**最大 fd + 1**，不是 fd 的个数
3. select 返回后会**改写**传进去的 fd_set，只留下就绪的位，所以每次循环都要重新设置
4. 限制：fd 号不能大于等于 `FD_SETSIZE`（本机 1024）；每次调用都把整个位图拷进内核、内核和用户态都要扫一遍，O(n)

---

## 任务

### 任务 A：select 版 echo 服务器

`server_select.c`：单线程，用一个数组记录所有客户端 fd。每轮循环：清空 fd_set，加入 listenfd 和所有客户端 fd，算出最大 fd，调用 select，
然后依次检查哪些 fd 就绪。

### 任务 B：撞 1024 的墙

1. 用第 2 节的 `many.c` 建 1100 个连接，看服务器在 fd 号到 1024 时发生什么。
2. Ubuntu 的 gcc 在 `-O2` 下默认开了 `_FORTIFY_SOURCE`，对 `FD_SET` 越界有运行时检查。分别用默认参数和 `-U_FORTIFY_SOURCE` 编译，对比现象。
   （不开检查时越界写的是栈上的别的变量，这是真实的内存破坏 bug，用 `make SAN=1` 看 ASan 怎么报。）

### 任务 C：量一量 O(n)

开 1000 个空闲连接，只有 1 个在不停收发。用 `perf stat` 或 `strace -c` 看服务器每次 select 的开销，和只开 10 个连接时比。

---

## 写之前先想清楚

1. 为什么 select 返回后必须重新 `FD_SET`？如果只在开头设置一次会怎样？
2. nfds 传错（比如传成连接数）会漏掉哪些 fd？
3. 1000 个连接里只有 1 个活跃，select 每轮要做多少无用功？用户态和内核各扫了几遍？

---

## 自查方法

- 多个 `nc` 同时连接都能回显，空闲时 CPU 接近 0。
- 能复现并解释 fd ≥ 1024 时的行为。
