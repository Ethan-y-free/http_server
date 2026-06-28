cmake_minimum_required(VERSION 3.10)
project(week01_echo_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 头文件路径（socket_raii.h 在这里）----
include_directories(${CMAKE_SOURCE_DIR})

# ==========================================================
# 目标 1：阻塞式 TCP Echo Server
# 编译：cd build && cmake .. && make tcp_server
# ==========================================================
add_executable(tcp_server tcp_server.cpp)

# ==========================================================
# 目标 2：阻塞式 TCP Echo Client
# ==========================================================
add_executable(tcp_client tcp_client.cpp)

# ==========================================================
# 目标 3：epoll 多客户端并发 Echo Server
# ==========================================================
add_executable(epoll_echo_server epoll_echo_server.cpp)

# ==========================================================
# 目标 4：epoll echo server v2（Epoll + Channel 封装版）
# ==========================================================
add_executable(epoll_echo_server_v2 epoll_echo_server_v2.cpp)

# ==========================================================
# 目标 5：epoll echo server v3（Buffer 集成 + 非阻塞写）
# ==========================================================
add_executable(epoll_echo_server_v3 epoll_echo_server_v3.cpp)
