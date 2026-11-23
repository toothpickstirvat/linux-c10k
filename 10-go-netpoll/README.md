# 第 10 节：Go的netpoll

## 为什么学

用Go写网络服务时，每个连接一个goroutine，代码是同步阻塞的写法，却能扛住几万连接。第2节的"一个连接一个线程"扛不住，
Go为什么可以？答案是Go在底层就是第7节的epoll ET + 非阻塞fd，只是把"读到EAGAIN就去等"这一步换成了挂起goroutine。

---

## 要学的内容（本机Go 1.27源码，`$(go env GOROOT)/src`）

1. `runtime/netpoll_epoll.go`：`netpollinit`里`EpollCreate1`；`netpollopen`注册
   `EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET`，一次注册读写，边沿触发
2. `internal/poll/fd_unix.go`的`(*FD).Read`：调用`read`，返回`EAGAIN`就调用`fd.pd.waitRead`
3. `runtime/netpoll.go`的`netpollblock`：把当前goroutine挂起（gopark），M和P去跑别的goroutine
4. `runtime/netpoll_epoll.go`的`netpoll`：调度器在找活干时调用`epoll_wait`，把就绪fd上挂着的goroutine重新放回运行队列

---

## 任务

### 任务 A：Go echo服务器

`main.go`：`net.Listen` + 每个连接一个goroutine `io.Copy(conn, conn)`。用第8节的压测客户端压1万连接，和第9节的C Reactor对比。

### 任务 B：用strace看它在做什么

`strace -f -e trace=epoll_create1,epoll_ctl,epoll_wait,read,write`运行，看：

- 每个新连接的`epoll_ctl`用的是什么flag？
- 某次`read`返回`EAGAIN`之后，紧接着发生了什么？
- 有几个线程在调用`epoll_wait`？

### 任务 C：看goroutine的开销

1万个连接时，`runtime.NumGoroutine()`和进程RSS各是多少？和第2节1万个线程对比，每个连接的内存差多少？

---

## 写之前先想清楚

1. Go的netpoll为什么选ET？它是怎么做到"读到EAGAIN"的？（提示：EAGAIN本来就是挂起goroutine的信号）
2. 一个goroutine读到EAGAIN挂起后，这个fd又来数据了。谁、在什么时候调用epoll_wait发现它的？
3. 文件I/O（读磁盘）为什么不走netpoll？读普通文件时goroutine会怎样？（联系go-concurrency第11节：系统调用时M和P分离）

---

## 自查方法

- 能对照strace输出，说清楚一次`conn.Read`从调用到返回经过了哪些系统调用。
- 能用一句话回答：Go的goroutine-per-connection和第2节的thread-per-connection，本质区别在哪里。
