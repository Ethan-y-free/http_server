cmake_minimum_required(VERSION 3.16)
project(http_server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Day 1+2: RAII Socket + 阻塞式 server/client
add_executable(tcp_server week01/tcp_server.cpp "week01/epoll_echo_server_v3.cpp"  "week01/eventloop.h" "week01/threadpool.h")
add_executable(tcp_client week01/tcp_client.cpp "week01/epoll_echo_server_v3.cpp"  "week01/eventloop.h" "week01/threadpool.h")

# Day 3: epoll 多客户端并发 echo server (LT 模式)
add_executable(epoll_echo_server week01/epoll_echo_server.cpp "week01/epoll_echo_server_v3.cpp"  "week01/eventloop.h" "week01/threadpool.h")
