/**
 * tcp_client.cpp —— 阻塞式 TCP Echo Client 完整参考
 *
 * 客户端流程（阶段0没做过）：
 *   Socket → Connect（三次握手在这里完成）→ 循环 send/recv
 *
 * 跟 server 的关键区别：
 *   - 没有 Bind/Listen/Accept，换成 Connect
 *   - stdin 读用户输入 → send 发给服务器 → recv 读回显
 *
 * 编译：g++ -std=c++17 tcp_client.cpp -o tcp_client
 * 运行：./tcp_client 127.0.0.1 8888
 */

#include <iostream>
#include <string>
#include <cstring>

#include "socket_raii.h"

namespace
{
    constexpr size_t BUF_SIZE = 4096;
}

int main (int argc, char* argv[])
{
    // ---- 1. 解析命令行参数 ----
    if (argc != 3)
    {
        std::cerr << "用法: " << argv[0] << " <ip> <port>" << std::endl;
        return 1;
    }

    std::string ip = argv[1];
    uint16_t port = static_cast<uint16_t> (std::stoi (argv[2]));

    try
    {
        // ---- 2. 创建 socket + 连接服务器 ----
        Socket client_sock;
        client_sock.Connect (ip, port);
        // Connect() 返回 = 三次握手完成 = 可以收发数据了

        std::cout << "已连接到 " << ip << ":" << port
                  << " (fd=" << client_sock.Fd () << ")" << std::endl;
        std::cout << "输入任意内容回车，Ctrl+C 退出\n" << std::endl;

        // ---- 3. 主循环：读 stdin → 发服务器 → 收回显 → 打印 ----
        std::string line;
        char buf[BUF_SIZE];

        while (true)
        {
            std::cout << "> ";

            // 从键盘读一行
            if (!std::getline (std::cin, line))
            {
                // getline 失败（Ctrl+D / EOF）
                std::cout << "\n再见" << std::endl;
                break;
            }

            if (line.empty ())
            {
                continue;  // 空行跳过，不发
            }

            // 发送到服务器（注意：加 \n 让服务器 echo 回来时保持换行）
            std::string send_msg = line + "\n";
            ssize_t sent = client_sock.Send (send_msg.c_str (), send_msg.size ());
            if (sent < 0)
            {
                std::cerr << "[ERROR] send() failed: "
                          << std::strerror (errno) << std::endl;
                break;
            }

            // 接收服务器回显
            std::memset (buf, 0, BUF_SIZE);
            ssize_t n = client_sock.Recv (buf, BUF_SIZE - 1);

            if (n < 0)
            {
                std::cerr << "[ERROR] recv() failed: "
                          << std::strerror (errno) << std::endl;
                break;
            }
            else if (n == 0)
            {
                std::cout << "服务器关闭了连接" << std::endl;
                break;
            }

            // 打印回显（去掉末尾换行）
            std::string echo (buf, n);
            while (!echo.empty () && (echo.back () == '\n' || echo.back () == '\r'))
            {
                echo.pop_back ();
            }
            std::cout << echo << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what () << std::endl;
        return 1;
    }

    return 0;
}
