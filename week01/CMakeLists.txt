cmake_minimum_required(VERSION 3.10)
project(week01_echo_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 头文件路径
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# 公共头文件
set(HEADERS
    socket_raii.h
    epoll.h
    channel.h
    buffer.h
    eventloop.h
    threadpool.h
)

# ==========================================================
# 目标 1：阻塞式 TCP Echo Server
# ==========================================================
add_executable(tcp_server tcp_server.cpp ${HEADERS})

# ==========================================================
# 目标 2：阻塞式 TCP Echo Client
# ==========================================================
add_executable(tcp_client tcp_client.cpp ${HEADERS})

# ==========================================================
# 目标 3：epoll 多客户端并发 Echo Server（LT 模式）
# ==========================================================
add_executable(epoll_echo_server epoll_echo_server.cpp ${HEADERS})

# ==========================================================
# 目标 4：epoll echo server v2（Epoll + Channel 封装版）
# ==========================================================
add_executable(epoll_echo_server_v2 epoll_echo_server_v2.cpp ${HEADERS})

# ==========================================================
# 目标 5：epoll echo server v3（Buffer 集成 + 非阻塞写）
# ==========================================================
add_executable(epoll_echo_server_v3 epoll_echo_server_v3.cpp ${HEADERS})

# ==========================================================
# 目标 6：单 Reactor Echo Server（EventLoop + ThreadPool）
# ==========================================================
add_executable(single_reactor_server single_reactor_server.cpp ${HEADERS})

# ==========================================================
# 目标 7：主从 Reactor Echo Server（MainReactor + SubReactor）
# ==========================================================
add_executable(multi_reactor_server multi_reactor_server.cpp ${HEADERS})

# ==========================================================
# 目标 8：主从 Reactor HTTP Server（集成 HTTP 解析）
# ==========================================================
add_executable(multi_reactor_http multi_reactor_http.cpp ${HEADERS} http_request.h http_parser.h http_static_handler.h)

# ==========================================================
# 目标 9：主从 Reactor HTTP Server（优化参考版 — 无锁架构）
# ==========================================================
add_executable(multi_reactor_http_full reference/multi_reactor_http_full.cpp ${HEADERS} http_request.h http_parser.h)

# ==========================================================
# 目标 10：单 Reactor HTTP Server（压测对比用）
# ==========================================================
add_executable(single_reactor_http single_reactor_http.cpp ${HEADERS} http_request.h http_parser.h)
