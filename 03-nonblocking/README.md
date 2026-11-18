# 第 3 节：非阻塞 I/O

## 为什么学

不想为每个连接开线程，就得让**一个线程**服务所有连接。前提是 `accept`、`read` 不能阻塞：没数据就立刻返回，去看下一个连接。
这一节先用最笨的办法实现，看清它为什么不行，下一节的 select 就是来解决它的。

---

## 要学的内容

1. `fcntl(fd, F_SETFL, flags | O_NONBLOCK)`，或者 `accept4(..., SOCK_NONBLOCK)`、`socket(..., SOCK_NONBLOCK, ...)`
2. 非阻塞时没数据：`read` 返回 -1，`errno` 是 `EAGAIN`（和 `EWOULDBLOCK` 是同一个值）
3. 非阻塞 `write`：发送缓冲区满了也返回 `EAGAIN`，没写完的数据要自己保存
4. 非阻塞 `connect`：返回 `EINPROGRESS`，连接在后台建立

---

## 任务

### 任务 A：轮询版服务器

`server_spin.c`：listenfd 和所有 connfd 都设成非阻塞，放进一个数组。主循环：

1. 试一次 `accept`，有新连接就加进数组；
2. 遍历数组，每个 fd 试一次 `read`，读到数据就写回，读到 0 就关闭并移出数组，`EAGAIN` 就跳过。

### 任务 B：看 CPU

- 没有任何客户端时，`top -p <pid>` 看 CPU 占用。
- 用 `strace -c -p <pid>` 跟 3 秒，看每秒调用了多少次 `accept4`/`read`，大部分返回什么。
- 在循环里加 `usleep(1000)` 后再看：CPU 降了，但代价是什么？

---

## 写之前先想清楚

1. `EAGAIN` 是错误吗？什么情况下 `read` 返回 -1 才是真的出错？
2. 加 `usleep` 能降 CPU，为什么它不是好办法？延迟和 CPU 之间是什么关系？
3. 我们真正想要的是什么：一个能"同时等很多 fd，任何一个有事就叫醒我"的系统调用。

---

## 自查方法

- 多个 `nc` 同时连接都能回显，只用一个线程（`ps -o nlwp` 是 1）。
- 能说清楚空闲时 CPU 100% 的原因。
