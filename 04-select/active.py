# 第 4 节 任务 C：一个"活跃"客户端，测 echo 往返延迟
#
# 用法：python3 04-select/active.py [往返次数] [端口]
#   python3 04-select/active.py              默认 20000 次，端口 9000
#   python3 04-select/active.py 50000 9000
#
# 连上服务器后，循环"发 2 个字节、收回 2 个字节"，算平均每次往返多少微秒。
# 配合 many 建立大量空闲连接，比较空闲连接数不同时，这 1 个活跃连接的延迟变化，
# 就能看出 select 的开销随连接总数增长（O(n)）。

import socket
import sys
import time

n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000

s = socket.create_connection(("127.0.0.1", port))
# 关掉 Nagle 算法：否则小包会被攒起来再发，测出来的是 Nagle 的等待时间，不是服务器的处理时间
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

start = time.perf_counter()
for _ in range(n):
    s.sendall(b"x\n")
    got = b""
    while len(got) < 2:  # TCP 是字节流，回显可能分几次到达，收满 2 个字节才算一次往返
        chunk = s.recv(2 - len(got))
        if not chunk:
            sys.exit("服务器关闭了连接")
        got += chunk
elapsed = time.perf_counter() - start

print(f"{n} 次往返用时 {elapsed:.2f} 秒，平均 {elapsed / n * 1e6:.1f} 微秒/次")
s.close()
