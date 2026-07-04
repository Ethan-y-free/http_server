cmake_minimum_required(VERSION 3.10)
project(mini_http_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(http_server
    multi_reactor_http.cpp
    socket_raii.h
    epoll.h
    channel.h
    buffer.h
    eventloop.h
    threadpool.h
    http_request.h
    http_parser.h
    http_static_handler.h
    timer_wheel.h
)

target_link_libraries(http_server pthread)
