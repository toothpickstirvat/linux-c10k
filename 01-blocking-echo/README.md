# 第 1 节：阻塞 echo 服务器

## 为什么从这里开始

后面所有的模型（多线程、select、epoll）解决的都是同一个问题：**一个线程调用`accept`或`read`时会阻塞，阻塞期间它什么都做不了。**
先亲手写一个最简单的阻塞服务器，看清楚它卡在哪里。

---

## 要学的内容

1. 服务端的五步：`socket` → `setsockopt(SO_REUSEADDR)` → `bind` → `listen` → 循环`accept`
2. 地址结构：`struct sockaddr_in`、`htons`/`htonl`、`inet_pton`/`inet_ntop`
3. `read`的三种返回值：大于0（读到数据）、等于0（对方关闭）、-1（出错，看`errno`）
4. `write`可能只写了一部分（short write），要循环写完
5. 用`strace`看程序每一步调用了哪个系统调用、阻塞在哪里

---

## 任务

### 任务 A：单连接echo服务器

写`server.c`：监听`0.0.0.0:9000`，`accept`一个连接后，循环读数据、原样写回，直到对方关闭，再去`accept`下一个。

- 每个连接建立时打印对方的IP和端口，关闭时打印"断开"。
- 所有系统调用都检查返回值。`socket`/`bind`/`listen`出错用`perror`打印并退出；`accept`、`read`出错时想一想：是退出整个程序，还是只关掉这个连接？
- 写一个`write_all(int fd, const char *buf, size_t n)`，循环直到n个字节全部写完。

测试：

```bash
cmake --preset default                            # 只需第一次
cmake --build --preset default && ./build/01-blocking-echo/server
nc 127.0.0.1 9000          # 另一个终端，输入什么回显什么
```

> **答：** 启动阶段（`socket`/`setsockopt`/`bind`/`listen`）失败，服务器根本没法工作，直接退出。
> 运行阶段某个连接出错（`accept`失败、`read`返回`ECONNRESET`、`write`返回`EPIPE`），只影响这一个客户端，
> 关掉这个连接、继续服务下一个，不能让一个客户端的问题把整个服务器带走。

### 任务 B：观察阻塞

1. 开两个`nc`同时连上。第二个输入内容，有没有回显？第一个断开之后呢？

   > **答：** 第二个没有回显。第一个断开后，第二个立刻收到之前输入内容的回显。
   > 服务器在内层循环里一直为第一个客户端阻塞在`read`上，根本没有回到`accept`。

2. 用`strace -f -e trace=network,read,write ./build/01-blocking-echo/server`运行，看程序停在哪个系统调用上。

   > **答：** 依次看到`socket`、`setsockopt`、`bind`、`listen`，然后停在没写完的`accept(3, ...`上（进程在内核里睡眠，等新连接）。
   > 第一个客户端连上后，`accept`返回`4`，接着停在`read(4, `上。第二个客户端连上并发数据时，**strace 没有任何新输出**，
   > 进程还停在`read(4, `，完全不知道有新连接。第一个断开时依次出现：`read`返回0 → `close(4)` →
   > `accept`立刻返回（第二个连接早在队列里了）→ `read`马上读到之前积压的数据。

3. 第二个`nc`没被accept时，用`ss -tnp 'sport = :9000'`看它的连接状态。它是ESTABLISHED吗？为什么还没accept就已经建立了？

   > **答：** 是`ESTAB`。实测输出：
   >
   > ```
   > State Recv-Q Send-Q Local Address:Port Peer Address:Port Process
   > ESTAB 7      0          127.0.0.1:9000    127.0.0.1:38858                                    ← 第二个，在队列里
   > ESTAB 0      0          127.0.0.1:9000    127.0.0.1:38844 users:(("server",pid=323319,fd=4)) ← 第一个，正在服务
   > ```
   >
   > - **为什么已经建立：** `listen()`之后，三次握手完全由内核完成，不需要进程参与。握手完成的连接放进**全连接队列**，
   >   `accept`只是从队列里取走一个、分配一个fd，**不负责建立连接**。
   > - **`Recv-Q 7`是什么：** 就是输入的`second\n`。数据已经到了服务器，存在内核为这个连接准备的**接收缓冲区**里，
   >   内核也已经回了ACK，所以客户端`write`早就成功返回了。`ss -tnm`能看到`skmem:(r967,rb131072,...)`：
   >   缓冲区上限约128KB，7字节数据占了967字节，多出的是内核包装数据包的`sk_buff`开销。
   > - **为什么`Process`一栏是空的：** `ss -p`靠扫描各进程的`/proc/<pid>/fd/`找socket属于谁。没被accept的连接
   >   还没有任何进程的fd指向它，所以是空的；被accept之后就显示`fd=4`。
   > - 对监听socket用`ss -tln`能直接看到这个队列：`LISTEN 1 4096`，`Recv-Q 1`是队列里等待accept的连接数，
   >   `Send-Q 4096`是队列上限。

### 加分项（选读，汇编）

libc是程序运行后才加载的，所以要先停在`main`，再反汇编`read`：

```bash
gdb -batch -ex 'break main' -ex run -ex 'disas read' ./build/01-blocking-echo/server
```

找到libc的`read`里那条`syscall`指令。看它前面怎么把系统调用号放进`eax`、参数放进`rdi`/`rsi`/`rdx`。
这就是用户态进入内核的那一步，也是"系统调用有开销"的来源。

---

## 写之前先想清楚

1. 不设`SO_REUSEADDR`，服务器关掉后马上重启，`bind`会报什么错？为什么？（提示：TIME_WAIT）

   > **答：** 报`bind: Address already in use`。TCP规定**先发起关闭的一方**要在`TIME_WAIT`状态等待2×MSL
   > （Linux固定60秒），连接才真正消失，目的有两个：最后一个ACK丢了时还能重新回应对方重发的FIN；
   > 让网络里残留的旧包过期，不会混进之后同一地址端口的新连接。
   > 按Ctrl+C停服务器时，是服务器这边先关闭连接，`TIME_WAIT`留在服务器一侧，占着9000端口，`bind`就被拒绝。
   > 设了`SO_REUSEADDR`后，允许在端口上有`TIME_WAIT`连接时照样bind。
   > 验证：有客户端连着时停服务器，马上`ss -tan state time-wait '( sport = :9000 )'`能看到它；没有客户端时停服务器，
   > 不设这个选项也能马上重启，因为没有`TIME_WAIT`。

2. `listen(fd, backlog)` 的backlog是什么队列的长度？连接是在`accept`时建立的，还是在`accept`之前？

   > **答：** backlog是**全连接队列**（accept队列）的长度，实际上限取`backlog`和`/proc/sys/net/core/somaxconn`
   > （本机4096）中较小的那个。连接在`accept`**之前**就建立好了：
   >
   > ```
   > 客户端 connect()          服务器内核                     服务器进程
   >    ── SYN ─────────────→  放进半连接队列（SYN 队列）
   >    ←──────── SYN+ACK ──
   >    ── ACK ─────────────→  移到全连接队列，状态 ESTAB      还在 read(4, ...)，不知道
   >                                                         accept() 从队列里取走一个
   > ```
   >
   > 全连接队列满了以后，内核会丢掉新来的握手包，客户端的`connect`会卡住并重传SYN，第8节压测时会遇到。
   > 半连接队列的长度由`tcp_max_syn_backlog`（本机1024）控制。

3. 客户端断开时`read`返回0。如果客户端断开后服务端还往这个连接`write`，会发生什么？（提示：SIGPIPE，进程会直接退出）

   > **答：** 用一个Python客户端一次发8KB后立刻`close`（服务器要分8次`read`/`write`），对比两个版本：
   >
   > ```
   > === 现在的 server（signal(SIGPIPE, SIG_IGN)）
   > 客户端 127.0.0.1:32842 已连接（fd=4）
   > write: Broken pipe              ← write 返回 -1、errno 为 EPIPE，只断开这个连接，进程还活着
   >
   > === 删掉 signal(SIGPIPE, SIG_IGN) 的版本
   > 客户端 127.0.0.1:44346 已连接（fd=4）
   > 进程已退出，状态码 141           ← 128+13，被 SIGPIPE 杀死，没有任何错误输出
   > ```
   >
   > 过程：客户端`close`发出FIN → 服务器缓冲区里还有数据，`read`继续读到数据而不是0 → 第1次`write`成功（只是拷进内核发送缓冲区）
   > → 客户端的socket已经没了，它的内核回RST → 第2次`write`往已被重置的连接写，内核发`SIGPIPE`，默认动作是**直接终止进程**。
   > 所以服务器一定要`signal(SIGPIPE, SIG_IGN)`，或者每次写用`send(fd, buf, n, MSG_NOSIGNAL)`。
   > 注意：在客户端那边测（关服务器、客户端再写）看到nc退出，是因为nc读到了EOF，和这里不是一回事。

4. `read`返回-1且`errno == EINTR`是什么意思？要不要当成错误？

   > **答：** 进程阻塞在系统调用里时，收到了一个**设置了处理函数**的信号，内核先去执行处理函数，让这次系统调用
   > 提前返回-1、`errno = EINTR`，此时**没有读写任何数据**。它不是错误，直接重试即可（代码里的`continue`）。
   > 信号被忽略（`SIG_IGN`）或是默认动作时不会出现EINTR，所以第1节其实碰不到；第2节fork版用`SIGCHLD`
   > 处理函数回收子进程，子进程一退出，阻塞的`accept`就会返回EINTR。也可以用`sigaction`加`SA_RESTART`让内核自动重启系统调用。

---

## 自查方法

```bash
cmake --build --preset default --target 01_server
./build/01-blocking-echo/server &
printf 'hello\n' | nc -q1 127.0.0.1 9000     # 应该回显 hello
kill %1
```

- 一个客户端时正常回显；两个客户端时第二个要等第一个断开才有回显。
- 服务器 Ctrl+C 后马上重启，`bind`不报错。
- 客户端断开后，服务器打印"断开"并继续等下一个连接，不会退出。

---

## 结论

1. **阻塞模型一次只能服务一个客户端。** 服务器为当前客户端阻塞在`read`时，新连接只能在全连接队列里排队，
   发来的数据只能堆在内核接收缓冲区里。这就是后面所有模型（多线程、select、epoll）要解决的问题。
2. **建立连接、收发数据包都是内核做的。** `accept`只是从队列取连接，`read`/`write`只是在用户缓冲区和内核缓冲区之间拷贝数据。
   后面select/epoll说的"可读"，意思就是"接收缓冲区里有数据"或"全连接队列里有连接"。
3. **`write`成功不等于对方收到了。** 它只表示数据进了本机的内核发送缓冲区。
4. **fd号会被立刻复用。** 每个连接`close`后，下一个`accept`返回的还是4。

## 踩过的坑

1. **每读一次就回去accept：** 最初只有一层循环，`read`一次后`continue`回到`accept`，第一个客户端再也没人读，
   它的fd也没关（泄漏）。要两层循环：外层accept，内层服务当前客户端。
2. **`write_all`覆盖而不是累加：** `written = write(...)`，部分写时下一次从错误的位置开始写；`write`返回-1时，
   和`size_t`比较被转成很大的无符号数，循环退出，错误被吞掉。编译器的`-Wsign-compare`就是在提醒这件事。
3. **`setsockopt`的值写成了0：** `socklen_t one = {0}`等于关闭`SO_REUSEADDR`。要写`int one = 1`，`socklen_t`只用于长度参数。
4. **同一个fd关两次：** 单线程只会得到`EBADF`，多线程里第二次`close`可能关掉别的线程刚accept、复用了同一fd号的连接。每个fd只在一个地方关。
5. **`read`出错不`break`：** 非EINTR的错误会无限循环打印`perror`。
6. **`printf("%s", buf)`：** `buf`不以`\0`结尾，还带着上次的残留（先发`hello`再发`hi`，打印成`hi\nlo\n`）。要用`%.*s`只打印n个字节。
7. **重定向后日志是空的：** stdout输出到终端时是行缓冲，输出到文件/管道时是全缓冲（攒满4KB才写），进程被信号杀死时
   缓冲区直接丢失。看日志时用`stdbuf -oL`，或代码里`setvbuf(stdout, NULL, _IOLBF, 0)`，或者打到不缓冲的stderr。

## 最佳实践

1. 系统调用返回什么类型就用什么类型接：`read`/`write`返回`ssize_t`，长度用`size_t`。编译开`-Wall -Wextra`并清掉所有warning。
2. 所有读写都要考虑"只完成了一部分"：写用`write_all`，读要按需要的字节数循环（TCP是字节流，不保证一次读到对方一次发的全部内容）。
3. 服务器启动时就`signal(SIGPIPE, SIG_IGN)`。
4. 区分"整个服务器出错"和"某个连接出错"：前者退出，后者只关这个连接。
5. `EINTR`一律重试。
6. `accept`的地址长度参数每次调用前都要重新赋值（它既是输入也是输出）。
