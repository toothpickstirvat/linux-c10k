# 第 1 节：阻塞 echo 服务器

## 为什么从这里开始

后面所有的模型（多线程、select、epoll）解决的都是同一个问题：**一个线程调用 `accept` 或 `read` 时会阻塞，阻塞期间它什么都做不了。**
先亲手写一个最简单的阻塞服务器，看清楚它卡在哪里。

---

## 要学的内容

1. 服务端的五步：`socket` → `setsockopt(SO_REUSEADDR)` → `bind` → `listen` → 循环 `accept`
2. 地址结构：`struct sockaddr_in`、`htons`/`htonl`、`inet_pton`/`inet_ntop`
3. `read` 的三种返回值：大于 0（读到数据）、等于 0（对方关闭）、-1（出错，看 `errno`）
4. `write` 可能只写了一部分（short write），要循环写完
5. 用 `strace` 看程序每一步调用了哪个系统调用、阻塞在哪里

---

## 任务

### 任务 A：单连接 echo 服务器

写 `server.c`：监听 `0.0.0.0:9000`，`accept` 一个连接后，循环读数据、原样写回，直到对方关闭，再去 `accept` 下一个。

- 每个连接建立时打印对方的 IP 和端口，关闭时打印"断开"。
- 所有系统调用都检查返回值，出错用 `perror` 打印并退出（`accept`、`read` 出错时想一想：是退出整个程序，还是只关掉这个连接？）。
- 写一个 `write_all(int fd, const char *buf, size_t n)`，循环直到 n 个字节全部写完。

测试：

```bash
make 01-blocking-echo/server && ./01-blocking-echo/server
nc 127.0.0.1 9000          # 另一个终端，输入什么回显什么
```

### 任务 B：观察阻塞

1. 开两个 `nc` 同时连上。第二个输入内容，有没有回显？第一个断开之后呢？
2. 用 `strace -f -e trace=network,read,write ./01-blocking-echo/server` 运行，看程序停在哪个系统调用上。
3. 第二个 `nc` 没被 accept 时，用 `ss -tnp 'sport = :9000'` 看它的连接状态。它是 ESTABLISHED 吗？为什么还没 accept 就已经建立了？

### 加分项（选读，汇编）

用 `gdb -batch -ex 'disas read' ./01-blocking-echo/server`（或 `objdump -d` 看 libc），找到 libc 的 `read` 里那条 `syscall` 指令。
看它前面怎么把系统调用号放进 `eax`、参数放进 `rdi`/`rsi`/`rdx`。这就是用户态进入内核的那一步，也是"系统调用有开销"的来源。

---

## 写之前先想清楚

1. 不设 `SO_REUSEADDR`，服务器关掉后马上重启，`bind` 会报什么错？为什么？（提示：TIME_WAIT）
2. `listen(fd, backlog)` 的 backlog 是什么队列的长度？连接是在 `accept` 时建立的，还是在 `accept` 之前？
3. 客户端断开时 `read` 返回 0。如果客户端断开后服务端还往这个连接 `write`，会发生什么？（提示：SIGPIPE，进程会直接退出）
4. `read` 返回 -1 且 `errno == EINTR` 是什么意思？要不要当成错误？

---

## 自查方法

```bash
make 01-blocking-echo/server
./01-blocking-echo/server &
printf 'hello\n' | nc -q1 127.0.0.1 9000     # 应该回显 hello
kill %1
```

- 一个客户端时正常回显；两个客户端时第二个要等第一个断开才有回显。
- 服务器 Ctrl+C 后马上重启，`bind` 不报错。
- 客户端断开后，服务器打印"断开"并继续等下一个连接，不会退出。
