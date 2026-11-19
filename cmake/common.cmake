# 所有课时共用的编译设置。
# 顶层 CMakeLists.txt 和每节的 CMakeLists.txt 都会 include 它，include_guard 保证只生效一次，
# 所以既可以在根目录统一构建，也可以单独构建某一节。
include_guard(GLOBAL)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# 生成 compile_commands.json，给 clangd / VS Code 做代码跳转和补全
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认 RelWithDebInfo：-O2 -g。
# 去掉 CMake 默认带的 -DNDEBUG，保证 assert 一直有效。
# 用 -O2 是因为 Ubuntu 的 gcc 只在优化级别 >= 1 时开启 _FORTIFY_SOURCE，第 4 节要用到。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "" FORCE)

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wshadow)

find_package(Threads REQUIRED)

# 两种 sanitizer 不能同时开
option(SAN  "开启 AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(TSAN "开启 ThreadSanitizer" OFF)
if(SAN AND TSAN)
  message(FATAL_ERROR "SAN 和 TSAN 不能同时开启，请分别用不同的构建目录")
endif()
if(SAN)
  # 编译选项排在 CMAKE_C_FLAGS_<CONFIG> 之后，这里的 -O1 会覆盖前面的 -O2
  add_compile_options(-O1 -fno-omit-frame-pointer -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()
if(TSAN)
  add_compile_options(-O1 -fno-omit-frame-pointer -fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

# 把当前目录下每个 .c 编译成一个同名可执行文件。
# 例：02-process-thread/server_thread.c -> 目标名 02_server_thread，文件名 server_thread
# CONFIGURE_DEPENDS：新建 .c 文件后直接 cmake --build 即可，CMake 会自动重新扫描，不用手动重新配置。
# 目标名加课号前缀，是因为不同课里会有同名文件（比如多个 server.c），目标名在整个项目里必须唯一。
function(c10k_add_all_sources)
  get_filename_component(lesson "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
  string(SUBSTRING "${lesson}" 0 2 num)
  file(GLOB srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
  foreach(src IN LISTS srcs)
    get_filename_component(stem "${src}" NAME_WE)
    set(target "${num}_${stem}")
    add_executable(${target} "${src}")
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${stem}")
    target_link_libraries(${target} PRIVATE Threads::Threads)
  endforeach()
endfunction()
