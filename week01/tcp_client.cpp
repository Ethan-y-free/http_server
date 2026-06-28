/**
 * tcp_client.cpp —— 阻塞式 TCP Echo Client
 *
 * 客户端是阶段0没做过的——从头理解 connect/send/recv 流程。
 * 注释=要求，代码留给你填。
 *
 * 编译：g++ -std=c++17 tcp_client.cpp -o tcp_client
 * 运行：./tcp_client <ip> <port>
 * 使用：输入任意文字回车，看 server echo 回来
 */

#include <iostream>
#include <string>
#include <cstring>

#include "socket_raii.h"

// TODO: 定义常量 BUF_SIZE=4096
constexpr size_t BUF_SIZE = 4096;


int main(int argc, char* argv[])
{
    // TODO: 1. 解析命令行参数（ip 和 port），参数不够打印用法后 return 1
    if (argc != 3)
    {
        std::cerr << "参数不够" << std::endl;
        return 1;
    }
    std::string ip = argv[1];
    uint16_t port = std::stoi(argv[2]);

    try
    {
        // TODO: 2. 创建 Socket client_sock
        Socket client_sock;

        // TODO: 3. client_sock.Connect(ip, port)
        //   只这一步，就完成了 TCP 三次握手 + 连接到服务器
        client_sock.Connect(ip, port);

        // TODO: 4. 打印"已连接到 ip:port"
        std::cout << "已连接到 " << ip << ":" << port
                  << " (fd=" << client_sock.Fd() << ")" << std::endl;
        std::cout << "输入任意内容回车，Ctrl+C 退出\n" << std::endl;

        // TODO: 5. 主循环：读 stdin 一行 → Send 到服务器 → Recv 回显 → 打印
        //   std::getline(std::cin, line);
        //   client_sock.Send(line.c_str(), line.size());
        //   ssize_t n = client_sock.Recv(buf, BUF_SIZE - 1);
        //   buf[n] = '\0';  → std::cout << buf << std::endl;
        std::string line;
        char buf[BUF_SIZE];
        while (std::getline(std::cin, line))
        {
            client_sock.Send(line.c_str(), line.size());
            std::memset(buf, 0, BUF_SIZE);
            ssize_t n = client_sock.Recv(buf, BUF_SIZE - 1);

            if (n < 0)
            {
                std::cerr << "[ERROR] recv() failed: "
                    << std::strerror(errno) << std::endl;
                break;
            }
            else if (n == 0)
            {
                std::cout << "服务器关闭了连接" << std::endl;
                break;
            }

            // 打印回显（去掉末尾换行）
            std::string echo(buf, n);
            while (!echo.empty() && (echo.back() == '\n' || echo.back() == '\r'))
            {
                echo.pop_back();
            }
            std::cout << echo << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
