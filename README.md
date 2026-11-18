# Linux网络I/O多路复用与C10K

从最简单的阻塞echo服务器开始，一步步走到epoll和Reactor，最后回到Go，看Go的netpoll怎么把epoll包装成"一个连接一个
goroutine"。

每一节都是你写代码，写完后检查并讲解问题。每节的 README 里有任务、思考题和自查方法；做完后补上实验结果、坑、最佳实践和思考题答案。

---

## 语言和工具

- **主要用 C（C17）**。socket、select、poll、epoll 都是 C 接口的系统调用，用 C 写最直接，没有额外的抽象挡在中间。
- **不用 C++**：C++ 的类和 RAII 能让代码更短，但会把"每个 fd 什么时候关、每个缓冲区什么时候释放"藏起来，而这正是这门课要看清楚的东西。
- **汇编只在第 1 节选读里出现一次**：用 `objdump`/`gdb` 看 libc 的 `read` 怎么通过 `syscall`
  指令进入内核，理解"系统调用的代价"从哪来。不需要写汇编。
- **第 10 节用 Go**：读 Go runtime 的 netpoll 源码，和上一门 go-concurrency 课程连起来。

工具（本机已有）：`gcc 15.2`、`make`、`gdb`、`strace`、`perf`、`ss`、`nc`、`objdump`。
没有 `wrk`、`ab` 这类压测工具，第 8 节自己写一个压测客户端，这本身就是练 epoll 的好机会。

### 编译

在项目根目录执行，可执行文件会放在源码旁边，`.gitignore` 已经排除了它们：

```bash
make 01-blocking-echo/server            # 编译 01-blocking-echo/server.c
make SAN=1 -B 01-blocking-echo/server   # 加 AddressSanitizer + UBSan，查内存错误
make TSAN=1 -B 02-process-thread/server_thread   # 加 ThreadSanitizer，查数据竞争
make clean                              # 删除所有可执行文件
```

---

## 本机环境（课程里的数字以这些为准）

| 项目                  | 值           | 和课程的关系                                                   |
|-----------------------|--------------|----------------------------------------------------------------|
| 内核                  | Linux 7.0    | epoll 的行为以这个版本为准                                     |
| CPU                   | 4 个逻辑 CPU | 第 8 节压测、第 9 节多 Reactor                                 |
| 内存                  | 15 GB        | 第 2 节"一个连接一个线程"能开多少个                            |
| `ulimit -n`           | 524288       | 一个进程最多打开的 fd 数，C10K 不会卡在这里                    |
| `ulimit -s`           | 8192 KB      | 每个线程默认栈 8 MB（虚拟内存），第 2 节要算                   |
| `threads-max`         | 98902        | 整个系统最多的线程数                                           |
| `FD_SETSIZE`          | 1024         | select 最多监视的 fd 号，第 4 节的核心限制                     |
| `ip_local_port_range` | 32768–60999  | 压测客户端到同一个服务端地址最多约 2.8 万个连接，第 8 节要用到 |
| `somaxconn`           | 4096         | `listen` 的 backlog 上限，瞬间涌入大量连接时相关               |

---

## 课程目录

### 第一阶段：阻塞模型和它的极限

| 目录                 | 主题                  | 核心内容                                                                        |
|----------------------|-----------------------|---------------------------------------------------------------------------------|
| `01-blocking-echo/`  | 阻塞 echo 服务器      | socket/bind/listen/accept/read/write，一次只能服务一个客户端；strace 看系统调用 |
| `02-process-thread/` | 一个连接一个进程/线程 | fork、pthread，C10K 问题的起点：内存和上下文切换                                |
| `03-nonblocking/`    | 非阻塞 I/O            | O_NONBLOCK、EAGAIN，轮询所有 fd 导致 CPU 100%，引出多路复用                     |

### 第二阶段：多路复用

| 目录           | 主题           | 核心内容                                                         |
|----------------|----------------|------------------------------------------------------------------|
| `04-select/`   | select         | fd_set、FD_SETSIZE=1024、每次重建集合、O(n) 扫描                 |
| `05-poll/`     | poll           | pollfd 数组、没有 1024 限制、仍然每次全量拷贝                    |
| `06-epoll-lt/` | epoll 水平触发 | epoll_create1/ctl/wait、就绪链表、只返回就绪的 fd                |
| `07-epoll-et/` | epoll 边沿触发 | 必须读到 EAGAIN、ET 下漏读导致连接卡死、EPOLLONESHOT、EPOLLRDHUP |

### 第三阶段：C10K

| 目录             | 主题          | 核心内容                                                                    |
|------------------|---------------|-----------------------------------------------------------------------------|
| `08-c10k-bench/` | 压测          | 自己写 epoll 压测客户端，1 万连接下对比线程/select/poll/epoll 的 CPU 和延迟 |
| `09-reactor/`    | Reactor       | 每连接读写缓冲、EPOLLOUT 写回压、超时（timerfd）、多线程 + SO_REUSEPORT     |
| `10-go-netpoll/` | Go 的 netpoll | 读 runtime 源码：Go 用 epoll ET + goroutine 挂起实现"同步写法、异步执行"    |

---

## 学习顺序

按目录编号顺序。每节只依赖前面的节。第 1–7 节每节一两个小程序；第 8 节的压测客户端后面几节都会用。

## 和上一门课的关联

- go-concurrency 第 5 节的 select 是 Go 语言的 channel 多路复用，这里的 select/poll/epoll 是操作系统的 fd
  多路复用，思路相同：同时等多个事件，哪个先就绪处理哪个。
- go-concurrency 第 11 节讲过 goroutine 阻塞在系统调用时 M 和 P 怎么分离。第 10 节会看到：网络 I/O 其实不会让 M 阻塞，Go 把
  fd 设成非阻塞，读到 EAGAIN 就把 goroutine 挂起，交给 epoll 等。
