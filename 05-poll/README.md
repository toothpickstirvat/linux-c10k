# 第 5 节：poll

## 为什么学

poll 改掉了 select 两个最难用的地方：没有 1024 的上限，传入和返回分开（`events` 和 `revents`），不用每次重建。
但它没解决 O(n)：每次调用仍然把整个数组拷进内核，内核仍然逐个检查。

---

## 要学的内容

1. `struct pollfd { int fd; short events; short revents; }`
2. `events`：`POLLIN`、`POLLOUT`；`revents` 里还可能出现 `POLLHUP`、`POLLERR`、`POLLNVAL`（这三个不用在 events 里申请）
3. fd 设成负数，poll 会跳过这一项，可以用来"删除"而不挪动数组
4. 和 select 的对比：上限、接口、拷贝量

---

## 任务

### 任务 A：poll 版 echo 服务器

`server_poll.c`：用一个可增长的 `struct pollfd` 数组（`realloc`）。连接关闭时怎么从数组里删掉？两种做法都试一下：
设成 -1 跳过，或者把最后一个元素挪过来。

### 任务 B：突破 1024

用 `many.c` 建 5000 个连接，确认服务器正常。和 select 版对比同样 1000 个连接时的 CPU 开销。

---

## 写之前先想清楚

1. 对方断开时，`revents` 里可能是 `POLLIN`（read 返回 0），也可能带 `POLLHUP`。两种都要处理吗？
2. 把最后一个元素挪过来填空，遍历 `revents` 时要注意什么？（提示：刚挪过来的那个还没检查）
3. poll 和 select 的 O(n) 是同一个 n 吗？select 的 n 是最大 fd 号，poll 的 n 是数组长度，什么时候差别大？

---

## 自查方法

- 5000 个连接正常回显。
- 连接反复建立、断开后，数组里没有残留的已关闭 fd（打印数组长度确认）。
