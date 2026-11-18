# 用法（在项目根目录执行）：
#   make 01-blocking-echo/server         编译 01-blocking-echo/server.c，可执行文件放在源码旁边
#   make SAN=1 -B 01-blocking-echo/server  加 AddressSanitizer + UBSan，查越界、use-after-free
#   make TSAN=1 -B 02-process-thread/server_thread  加 ThreadSanitizer，查多线程数据竞争
#   make clean                           删除所有和 .c 同名的可执行文件
#
# 没有为每个程序写规则：靠 make 内置的 "%: %.c" 规则，这里只设置编译参数。
# 注意：Ubuntu 的 gcc 在 -O2 下默认开启 _FORTIFY_SOURCE=3，第 4 节 select 会用到这一点。

CC      := gcc
CFLAGS  := -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wshadow -g -O2
LDLIBS  := -pthread

ifeq ($(SAN),1)
CFLAGS  += -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
endif

ifeq ($(TSAN),1)
CFLAGS  += -O1 -fno-omit-frame-pointer -fsanitize=thread
LDFLAGS += -fsanitize=thread
endif

.PHONY: clean
clean:
	@for f in */*.c; do [ -f "$${f%.c}" ] && rm -f "$${f%.c}"; done; true
