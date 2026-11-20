# 第 4 节：可以在根目录统一构建，也可以单独构建：
#   cmake -S 04-select -B build-04 && cmake --build build-04
cmake_minimum_required(VERSION 3.20)
project(c10k_04_select C)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/common.cmake)

# 本目录下每个 .c 文件编译成一个同名可执行文件
c10k_add_all_sources()

# 任务：分别用默认参数和 -U_FORTIFY_SOURCE 编译 select 服务器，对比 FD_SET 越界时的现象。
# 写好 server_select.c 后会多出一个目标 04_server_select_nofortify。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server_select.c")
  add_executable(04_server_select_nofortify server_select.c
          server_select.c)
  set_target_properties(04_server_select_nofortify PROPERTIES OUTPUT_NAME server_select_nofortify)
  target_compile_options(04_server_select_nofortify PRIVATE -U_FORTIFY_SOURCE)
  target_link_libraries(04_server_select_nofortify PRIVATE Threads::Threads)
endif()
