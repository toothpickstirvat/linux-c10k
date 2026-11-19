# 第 3 节：非阻塞 I/O

## 为什么学

不想为每个连接开线程，就得让**一个线程**服务所有连接。前提是`accept`、`read`不能阻塞：没数据就立刻返回，去看下一个连接。
这一节先用最笨的办法实现，看清它为什么不行，下一节的select就是来解决它的。

---

## 要学的内容

1. `fcntl(fd, F_SETFL, flags | O_NONBLOCK)`，或者`accept4(..., SOCK_NONBLOCK)`、`socket(..., SOCK_NONBLOCK, ...)`
2. 非阻塞时没数据：`read`返回-1，`errno`是`EAGAIN`（和`EWOULDBLOCK`是同一个值）
3. 非阻塞`write`：发送缓冲区满了也返回`EAGAIN`，没写完的数据要自己保存
4. 非阻塞`connect`：返回`EINPROGRESS`，连接在后台建立

---

## 任务

### 任务 A：轮询版服务器

`server_spin.c`：listenfd和所有connfd都设成非阻塞，放进一个数组。主循环：

1. 试一次`accept`，有新连接就加进数组；
2. 遍历数组，每个fd试一次`read`，读到数据就写回，读到0就关闭并移出数组，`EAGAIN`就跳过。

### 任务 B：看 CPU

- 没有任何客户端时，`top -p <pid>` 看CPU占用。
- 用`strace -c`数2秒内调用了多少次`accept4`/`read`，大部分返回什么。
  本机`/proc/sys/kernel/yama/ptrace_scope`是1，`strace -p <pid>`附加不到已运行的进程，所以让strace直接启动服务器：
  ```bash
  timeout -s TERM 2 strace -c -o /tmp/st.txt ./build/03-nonblocking/server_spin; cat /tmp/st.txt
  ```
- 用`./build/03-nonblocking/server_spin 1000`（每轮`usleep(1000)`）再看：CPU降了，但代价是什么？

#### 实测结果（空闲，没有客户端）

| | CPU | 2 秒内 accept4 次数 | 其中返回 EAGAIN | echo 往返延迟 |
|---|---|---|---|---|
| 不休眠 | **100%** | 16531 | 16531（全部） | 66 微秒 |
| `usleep(1000)` | **2%** | 1239 | 1239（全部） | 1888 微秒 |

不休眠时，没有任何客户端，线程却把一个CPU核吃满：每一次`accept4`都返回`EAGAIN`，全是白跑。
加了`usleep(1000)`，CPU降到2%，但延迟涨了接近30倍：请求到达时线程可能正在睡，要等它醒来才轮询到。

```bash
# 亲眼看 EAGAIN：
timeout -s TERM 1 strace -e trace=accept4,read ./build/03-nonblocking/server_spin 2>&1 | head -3
# accept4(3, ..., SOCK_NONBLOCK) = -1 EAGAIN (Resource temporarily unavailable)
```

---

## 写之前先想清楚

1. `EAGAIN`是错误吗？什么情况下`read`返回-1才是真的出错？

   > **答：** 不是错误。`EAGAIN`（Linux上和`EWOULDBLOCK`是同一个值）的意思是"这个调用在阻塞模式下本来要阻塞，
   > 但fd是非阻塞的，所以立刻返回"，这次调用什么都没做，连接状态也没变。在socket上：
   > - `read`：接收缓冲区为空（对方关闭时返回0，不是EAGAIN）；
   > - `write`：发送缓冲区满，一个字节都写不进去（能写一部分时返回实际字节数）；
   > - `accept`：全连接队列为空。
   >
   > 非阻塞`connect`返回的是`EINPROGRESS`，不是EAGAIN。`read`返回-1时，`EAGAIN`（没数据）和`EINTR`（被信号打断）
   > 都不是真的错误；其他errno，比如`ECONNRESET`（对方发了RST）、`EBADF`，才是真的出错，要关掉这个连接。
2. 加`usleep`能降CPU，为什么它不是好办法？延迟和CPU之间是什么关系？

   > **答：** 睡眠时间就是额外延迟的上限：睡得越久越省CPU，请求等得越久；睡得越短延迟越低，CPU越高。
   > 实测`usleep(1000)`让CPU从100%降到2%，往返延迟却从66微秒涨到1888微秒。调这个值只能在"费CPU"和"高延迟"之间挪，
   > 没法两全。而且连接多了以后，每一轮都要对每个fd调一次`read`，就算睡眠时间不变，一轮的开销也随连接数线性增长。
3. 我们真正想要的是什么：一个能"同时等很多fd，任何一个有事就叫醒我"的系统调用。

   > **答：** 轮询的根本问题是方向反了：我们主动一遍遍问内核"有数据了吗"，内核每次都答"没有"。
   > 应该让内核在有事时主动通知：线程睡在一个系统调用里，同时等很多fd，任何一个可读、可写或有新连接就醒来，
   > 并告诉我们是哪几个。这样空闲时CPU是0，事件一到马上被唤醒，延迟也低。这就是第4节的`select`，以及后面的`poll`、`epoll`。

---

## 自查方法

- 多个`nc`同时连接都能回显，只用一个线程（`ps -o nlwp` 是 1）。
- 能说清楚空闲时CPU 100%的原因。

---

## 结论

1. **非阻塞让一个线程可以服务很多连接**，因为任何调用都不会把线程卡住。
2. **但只靠非阻塞 + 轮询不行**：空闲时忙等吃满一个CPU核；加休眠又换来延迟，CPU和延迟此消彼长。
3. 缺的是"等待"的能力：要一个系统调用能同时等多个fd、有事才醒。这就是I/O多路复用。

## 非阻塞的几个细节

1. `SOCK_NONBLOCK`只能在创建时用（`socket`、`accept4`），效果等于`fcntl(fd, F_SETFL, flags | O_NONBLOCK)`。
2. **accept出来的connfd不继承listenfd的非阻塞属性**，要用`accept4(..., SOCK_NONBLOCK)`或`fcntl`单独设。
   忘了设的话，轮询到这个连接时`read`会阻塞，其他连接全没人管。
3. 非阻塞标志属于"打开的文件"，不属于fd号：`dup`出来的fd、fork后子进程里的同一个fd共用这个标志。
4. 对普通磁盘文件没有作用：读磁盘照样等I/O完成，不会返回EAGAIN。

## 踩过的坑

1. **`strace -p`附加失败**：`ptrace_scope=1`时只能跟踪自己启动的子进程。要么让strace启动程序，要么`sudo`。
2. **用`$!`取pid量CPU得到0%**：`timeout 6 ./server &`时`$!`是`timeout`进程的pid，不是服务器。要用服务器自己的pid（`pgrep -x server_spin`）。

## 最佳实践

1. 非阻塞fd上，`EAGAIN`和`EINTR`都不当错误处理，其他errno才关连接。
2. 新连接一律用`accept4(..., SOCK_NONBLOCK)`，不要依赖继承。
3. 非阻塞`write`遇到`EAGAIN`时，正确做法是把没写完的数据存起来，等fd可写再写（第6节）。本节`write_all`原地重试是简化写法，只适合echo这种小数据。
4. 不要用"轮询 + sleep"代替等待，用select/poll/epoll。
