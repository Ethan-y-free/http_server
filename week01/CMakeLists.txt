cmake_minimum_required(VERSION 3.10)
project(week01_http_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==========================================================
# Build type
# ==========================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug | Release | RelWithDebInfo" FORCE)
endif()
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ==========================================================
# 头文件分组（header-only，不编译 .o）
# ==========================================================

# 核心：Socket + Epoll + Channel + Buffer + EventLoop + ThreadPool
set(CORE_HEADERS
    socket_raii.h
    epoll.h
    channel.h
    buffer.h
    eventloop.h
    threadpool.h
)

# HTTP：请求解析 + 静态文件处理
set(HTTP_HEADERS
    http_request.h
    http_parser.h
    http_static_handler.h
)

# 基础设施：时间轮 + 异步日志
set(INFRA_HEADERS
    timer_wheel.h
    async_logger/log_stream.h
    async_logger/log_buffer.h
    async_logger/async_log_writer.h
    async_logger/logger.h
)

# 全量头文件（multi_reactor_http 使用）
set(ALL_HEADERS ${CORE_HEADERS} ${HTTP_HEADERS} ${INFRA_HEADERS})

# ==========================================================
# 可执行目标
# ==========================================================

# ---- 基础 TCP Echo ----
add_executable(tcp_server tcp_server.cpp)
add_executable(tcp_client tcp_client.cpp)

# ---- Epoll Echo Server（LT / 封装 / Buffer）----
add_executable(epoll_echo_server    epoll_echo_server.cpp    ${CORE_HEADERS})
add_executable(epoll_echo_server_v2 epoll_echo_server_v2.cpp ${CORE_HEADERS})
add_executable(epoll_echo_server_v3 epoll_echo_server_v3.cpp ${CORE_HEADERS})

# ---- Reactor Echo Server（单 / 主从）----
add_executable(single_reactor_server single_reactor_server.cpp ${CORE_HEADERS})
add_executable(multi_reactor_server  multi_reactor_server.cpp  ${CORE_HEADERS})

# ---- HTTP Server ----
add_executable(single_reactor_http single_reactor_http.cpp ${CORE_HEADERS} ${HTTP_HEADERS})
add_executable(multi_reactor_http  multi_reactor_http.cpp  ${ALL_HEADERS})

# ---- HTTP Server 参考版（reference 目录下的完整实现）----
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/reference/multi_reactor_http_full.cpp")
    add_executable(multi_reactor_http_full
        reference/multi_reactor_http_full.cpp
        ${ALL_HEADERS}
    )
endif()

# ---- 所有目标统一链接 ----
find_package(Threads REQUIRED)
set(ALL_TARGETS
    tcp_server tcp_client
    epoll_echo_server epoll_echo_server_v2 epoll_echo_server_v3
    single_reactor_server multi_reactor_server
    single_reactor_http multi_reactor_http
)
foreach(target IN LISTS ALL_TARGETS)
    target_link_libraries(${target} PRIVATE Threads::Threads)
endforeach()

# multi_reactor_http_full 单独处理（可能不在列表中）
if(TARGET multi_reactor_http_full)
    target_link_libraries(multi_reactor_http_full PRIVATE Threads::Threads)
endif()

# ==========================================================
# 复制 www 目录到构建目录
# ==========================================================
add_custom_command(
    TARGET multi_reactor_http POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/www"
        "$<TARGET_FILE_DIR:multi_reactor_http>/www"
    COMMENT "Copying www/ → build dir"
)
