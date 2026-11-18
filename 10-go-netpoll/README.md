# 第 10 节：Go 的 netpoll

## 为什么学

用 Go 写网络服务时，每个连接一个 goroutine，代码是同步阻塞的写法，却能扛住几万连接。第 2 节的"一个连接一个线程"扛不住，
Go 为什么可以？答案是 Go 在底层就是第 7 节的 epoll ET + 非阻塞 fd，只是把"读到 EAGAIN 就去等"这一步换成了挂起 goroutine。

---

## 要学的内容（本机 Go 1.27 源码，`$(go env GOROOT)/src`）

1. `runtime/netpoll_epoll.go`：`netpollinit` 里 `EpollCreate1`；`netpollopen` 注册 `EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET`，一次注册读写，边沿触发
2. `internal/poll/fd_unix.go` 的 `(*FD).Read`：调用 `read`，返回 `EAGAIN` 就调用 `fd.pd.waitRead`
3. `runtime/netpoll.go` 的 `netpollblock`：把当前 goroutine 挂起（gopark），M 和 P 去跑别的 goroutine
4. `runtime/netpoll_epoll.go` 的 `netpoll`：调度器在找活干时调用 `epoll_wait`，把就绪 fd 上挂着的 goroutine 重新放回运行队列

---

## 任务

### 任务 A：Go echo 服务器

`main.go`：`net.Listen` + 每个连接一个 goroutine `io.Copy(conn, conn)`。用第 8 节的压测客户端压 1 万连接，和第 9 节的 C Reactor 对比。

### 任务 B：用 strace 看它在做什么

`strace -f -e trace=epoll_create1,epoll_ctl,epoll_wait,read,write` 运行，看：

- 每个新连接的 `epoll_ctl` 用的是什么 flag？
- 某次 `read` 返回 `EAGAIN` 之后，紧接着发生了什么？
- 有几个线程在调用 `epoll_wait`？

### 任务 C：看 goroutine 的开销

1 万个连接时，`runtime.NumGoroutine()` 和进程 RSS 各是多少？和第 2 节 1 万个线程对比，每个连接的内存差多少？

---

## 写之前先想清楚

1. Go 的 netpoll 为什么选 ET？它是怎么做到"读到 EAGAIN"的？（提示：EAGAIN 本来就是挂起 goroutine 的信号）
2. 一个 goroutine 读到 EAGAIN 挂起后，这个 fd 又来数据了。谁、在什么时候调用 epoll_wait 发现它的？
3. 文件 I/O（读磁盘）为什么不走 netpoll？读普通文件时 goroutine 会怎样？（联系 go-concurrency 第 11 节：系统调用时 M 和 P 分离）

---

## 自查方法

- 能对照 strace 输出，说清楚一次 `conn.Read` 从调用到返回经过了哪些系统调用。
- 能用一句话回答：Go 的 goroutine-per-connection 和第 2 节的 thread-per-connection，本质区别在哪里。
