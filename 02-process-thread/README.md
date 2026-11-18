# 第 2 节：一个连接一个进程 / 一个线程

## 为什么学

第 1 节的服务器一次只能服务一个人。最直接的办法是每来一个连接就开一个进程或线程去处理。这个模型写起来最简单，
早期的 Apache 就是这样。它的问题就是 C10K 问题的起点：**连接数上万时，进程/线程本身的开销压垮了机器。**

---

## 要学的内容

1. `fork`：父进程继续 `accept`，子进程处理连接；父子进程各自要关掉不用的 fd
2. 僵尸进程：子进程退出后父进程不 `waitpid`，会留下僵尸；用 `SIGCHLD` 处理函数回收
3. `pthread_create` + `pthread_detach`：线程退出后自动回收
4. 每个线程的开销：栈（本机默认 8 MB 虚拟内存）、内核调度结构、上下文切换

---

## 任务

### 任务 A：fork 版

`server_fork.c`：每 accept 一个连接就 `fork`，子进程跑第 1 节的 echo 循环，结束后 `exit`。父进程用 `SIGCHLD` 回收子进程。

### 任务 B：线程版

`server_thread.c`：每 accept 一个连接就 `pthread_create`，线程函数跑 echo 循环，`pthread_detach` 让它自己回收。
注意：fd 怎么传给线程函数？（传 `&connfd` 有什么问题？）

### 任务 C：量一量开销

写一个小客户端 `many.c`：建立 N 个连接，全部保持不关，然后 `pause()`。对线程版服务器：

- N = 1000 时，用 `ps -o pid,nlwp,vsz,rss -p <pid>` 看线程数、虚拟内存、实际内存。
- 算一算：每个线程的 VSZ 和 RSS 各多少？为什么两者差这么多？
- 用 `ulimit -s 256` 启动服务器后再看，或者用 `pthread_attr_setstacksize` 把栈调小。

---

## 写之前先想清楚

1. fork 后，子进程要不要关 listenfd？父进程要不要关 connfd？不关会怎样？（提示：客户端断开后连接为什么不真正关闭）
2. 线程版里，一个线程 `close(fd)` 之后，fd 号被下一个 `accept` 复用。如果别的线程还拿着旧的 fd 号在读，会读到谁的数据？
3. 8 MB 栈 × 1 万个线程 = 80 GB，本机只有 15 GB 内存，为什么还可能开得出来？那真正的限制是什么？

---

## 自查方法

- 多个 `nc` 同时连接，都能各自回显。
- fork 版客户端全部断开后，`ps` 里没有 `<defunct>` 的僵尸进程。
- 线程版用 `make TSAN=1 -B 02-process-thread/server_thread` 编译（ThreadSanitizer），多个客户端并发收发时没有报 data race。
