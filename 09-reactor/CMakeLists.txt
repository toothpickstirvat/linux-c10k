# 第 9 节：可以在根目录统一构建，也可以单独构建：
#   cmake -S 09-reactor -B build-09 && cmake --build build-09
cmake_minimum_required(VERSION 3.20)
project(c10k_09_reactor C)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/common.cmake)

# 本目录下每个 .c 文件编译成一个同名可执行文件
c10k_add_all_sources()

