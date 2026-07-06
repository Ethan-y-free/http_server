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

# 编译后自动复制 www 目录到二进制所在目录
add_custom_command(TARGET http_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/www"
    "$<TARGET_FILE_DIR:http_server>/www")
