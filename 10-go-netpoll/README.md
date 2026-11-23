# 第 10 节：Go的netpoll

## 为什么学

用Go写网络服务时，每个连接一个goroutine，代码是同步阻塞的写法，却能扛住几万连接。第2节的“一个连接一个线程”扛不住，Go为什么可以？答案是Go在底层就是第7节的epoll
ET + 非阻塞fd，只是把“读到EAGAIN就去等”这一步换成了挂起goroutine。

---

## 要学的内容（本机Go 1.27.3源码，`$(go env GOROOT)/src`）

1. `runtime/netpoll_epoll.go`：`netpollinit`里`EpollCreate1`；`netpollopen`注册
   `EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET`，一次注册读写，边沿触发
2. `internal/poll/fd_unix.go`的`(*FD).Read`：调用`read`，返回`EAGAIN`就调用`fd.pd.waitRead`
3. `runtime/netpoll.go`的`netpollblock`：把当前goroutine挂起（gopark），M和P去跑别的goroutine
4. `runtime/netpoll_epoll.go`的`netpoll`：调度器在找活干时调用`epoll_wait`，把就绪fd上挂着的goroutine重新放回运行队列

---

## 任务

### 任务 A：Go echo服务器

`main.go`：`net.Listen` + 每个连接一个goroutine `io.Copy(conn, conn)`。`-stats`参数每秒打印goroutine数和连接数。构建：
`cd 10-go-netpoll && go build -o /tmp/echo_go .`（这一节是Go项目，没有CMakeLists.txt，用`go.mod`管理）。

#### 实测结果（N=10000，每连接每秒1次，同机回环，只测一轮）

|                          | p50     | p99                | 服务器CPU(tick) | 运行中RSS | 线程数 |
|--------------------------|---------|--------------------|-----------------|-----------|--------|
| Go echo                  | 750 µs  | 4320 µs            | 203             | 62.8 MB   | 8      |
| 第9节 C Reactor（1线程） | 798 µs  | 5343 µs            | 128             | 42.7 MB   | 1      |
| 第8节 epoll LT           | 652 µs  | 1616 µs            | 116             | 1.9 MB    | 1      |
| 第8节 线程版             | 1602 µs | 数百毫秒（波动大） | 746             | 85 MB     | 10001  |

- 1万个连接全部建成，回显80049次，运行中goroutine数为10002。Go的延迟和C Reactor同一量级，服务器CPU比C版高。
- loadgen在第9节改过消息格式，对原样回显的服务器无影响。只测了一轮，数值有波动，比例比绝对值可靠。

### 任务 B：用strace看它在做什么

让strace直接启动程序（本机`ptrace_scope=1`，不能`strace -p`）：
`strace -f -e trace=epoll_ctl,epoll_pwait,splice,read,write -o 文件 /tmp/echo_go`。

- **epoll_ctl的flag：** 每个新连接都是`EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET`，一次注册读写，边沿触发，和`netpollopen`
  源码一致。关闭时是`EPOLL_CTL_DEL`。
- **EAGAIN之后：** 实测`splice(4, ..., 8, ...)`返回`EAGAIN`后，线程没有重试，也没有睡在read里；过了一会儿`epoll_pwait`
  返回了该fd的`EPOLLIN|EPOLLOUT`，随后才再次`splice`并读到3字节。对方关闭时`splice`返回0，紧接着`epoll_ctl DEL`。
- **几个线程调用epoll_wait：** 不止一个，抓到3个不同线程调用过`epoll_pwait`（超时为0的是顺手查一下，超时为-1的是没活可干睡在里面），没有固定的专职线程。
- **新版Go用的是`epoll_pwait`不是`epoll_wait`**，strace过滤条件要写对。
- **io.Copy在Linux上走`splice`**（`net/tcpsock_posix.go`调用`spliceFrom`，见`net/splice_linux.go`
  ），所以只过滤read/write时，socket上的读写一个也看不到，EAGAIN也是0次。
- 没有核实：启动时`epoll_ctl ADD fd6 EPOLLIN data=0`很可能是运行时用来唤醒epoll_wait的fd，另有一次ADD随即DEL的fd，没有查源码，不下结论。

### 任务 C：看goroutine的开销

1万个连接，压测进行中采样：

|              | goroutine/线程数          | RSS     | 每连接RSS（粗算） |
|--------------|---------------------------|---------|-------------------|
| Go echo      | 10002个goroutine，8个线程 | 62.8 MB | 约6 KB            |
| 第2节 线程版 | 10001个线程               | 85 MB   | 约8.5 KB          |

- 粗算没有扣除进程自身基础占用，只能看量级：**两者RSS差别不大（Go略小），真正的区别是线程数：8个对10001个。**
- Go的RSS里还包含io.Copy缓冲、pollDesc等，没有拆开统计；没有量Go的VSZ。
- 延迟差别更明显：线程版p99是数百毫秒，Go约4.3 ms。

---

## 写之前先想清楚

1. Go的netpoll为什么选ET？它是怎么做到“读到EAGAIN”的？（提示：EAGAIN本来就是挂起goroutine的信号）

   > **答：** fd注册一次就够：`netpollopen`一次注册`EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET`，之后不用再`epoll_ctl MOD`
   ，通知也不会重复。ET的“必须读到EAGAIN”在Go里自然成立：`(*FD).Read`调用read，遇到`EAGAIN`（且fd是pollable）就`waitRead`，在
   `netpollblock`里gopark挂起goroutine。挂起期间fd又来数据，ET报告一次，goroutine被唤醒后再read。所以EAGAIN既是“读空了”的标志，也是“该挂起了”的信号。实测见任务B。

2. 一个goroutine读到EAGAIN挂起后，这个fd又来数据了。谁、在什么时候调用epoll_wait发现它的？

   > **答：** 没有专门的线程。调度器在找活干时调用`netpoll`：`runtime/proc.go`里多处调用，`netpoll(0)`非阻塞查一次，
   `netpoll(delay)`在没有可运行goroutine时阻塞等待，监控线程里还有一处非阻塞调用（约6622行）。`netpoll`
   返回就绪fd上挂着的goroutine列表，放回运行队列。实测多个线程都调用过`epoll_pwait`，超时有0也有-1，与此吻合。

3. 文件I/O（读磁盘）为什么不走netpoll？读普通文件时goroutine会怎样？（联系go-concurrency第11节：系统调用时M和P分离）

   > **答（推理得出，本节未实验）：**
   epoll不支持普通文件，磁盘文件总是“就绪”的，没有EAGAIN可等，没法靠epoll挂起。读普通文件时read真的阻塞在系统调用里，占住线程（M）；这时调度器把P从这个M上摘走交给别的M，继续跑其他goroutine。代价是阻塞的文件I/O会多消耗线程。

---

## 自查方法

- 对照strace说清一次`conn.Read`的过程：fd非阻塞 → read（这里io.Copy是splice）返回`EAGAIN` → `waitRead` → `netpollblock`
  gopark → 某个线程调度时`epoll_pwait`返回`EPOLLIN` → goroutine回到运行队列 → 再次读到数据。
- 一句话：
  **goroutine-per-connection和thread-per-connection的代码都是同步阻塞写法，区别在于“阻塞”的实现：线程版是内核挂起整个线程（1万连接就是1万个线程），Go是运行时用非阻塞fd +
  epoll ET，只挂起goroutine，几个线程就能服务1万个连接。**

---

## 结论

1. **Go的netpoll就是epoll ET + 非阻塞fd**，goroutine挂起代替了C里手写的状态机和事件循环。
2. 1万连接下Go只用8个线程，延迟和C Reactor同量级，远好于线程版。
3. 代价：服务器CPU比C Reactor高（203对128 tick，一轮测量）。
4. 内存上Go与线程版RSS差别不大，不能说goroutine省很多物理内存，省的是线程数和调度开销。

## 踩坑

1. **strace只过滤read/write：** io.Copy走splice，看不到socket读写，EAGAIN也是0次。
2. **epoll_wait在新版是epoll_pwait：** 过滤名要写对。
3. **Accept出错直接continue：** fd用完（EMFILE）时空转，要打印并稍等。
4. **`.gitignore`的`/*/*`会忽略go.mod：** 需要加`!/*/go.mod`放行。
5. **README被编辑器用旧内容覆盖：** 第8、10节的README提交时都只剩原始模板，提交前要用`git diff --cached`核对内容。

## 最佳实践

1. 验证运行时行为，strace和源码两边对照，不要只看一边。
2. 压测在运行中采样RSS、线程数、goroutine数，不要等结束后。
3. 比较内存时说明有没有扣除基础占用。
