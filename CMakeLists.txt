# 根目录统一构建第 1–9 节（第 10 节是 Go，用 go build，不在这里）。
# 用法见 CMakePresets.json 或 README.md 的"编译"一节。
cmake_minimum_required(VERSION 3.20)
project(linux_c10k C)

include(cmake/common.cmake)

add_subdirectory(01-blocking-echo)
add_subdirectory(02-process-thread)
add_subdirectory(03-nonblocking)
add_subdirectory(04-select)
add_subdirectory(05-poll)
add_subdirectory(06-epoll-lt)
add_subdirectory(07-epoll-et)
add_subdirectory(08-c10k-bench)
add_subdirectory(09-reactor)
