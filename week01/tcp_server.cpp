/**
 * tcp_server.cpp —— 阻塞式 TCP Echo Server（RAII Socket 版）
 *
 * 用你刚写好的 Socket 类重新实现 echo server。
 * 注释=要求，代码留给你填。
 *
 * 编译：g++ -std=c++17 tcp_server.cpp -o tcp_server
 * 运行：./tcp_server
 * 测试：nc localhost 8888
 */

#include <iostream>
#include <string>
#include <cstring>

#include "socket_raii.h"

// TODO: 定义常量 PORT=8888, BACKLOG=128, BUF_SIZE=4096
constexpr uint16_t PORT = 8888;
constexpr int BACKLOG = 128;
constexpr size_t BUF_SIZE = 4096;


/**
 * handle_client —— 处理单个客户端 echo 循环
 *   从 client_sock 循环 recv → 打印 → send 原样返回
 *   直到 recv 返回 0（客户端断开）或 <0（出错）
 *
 * recv() 返回 0   = 对端发了 FIN，正常关闭
 * recv() 返回 <0  = 出错
 * recv() 返回 >0  = 收到 n 字节
 */
// TODO: 实现 handle_client(Socket& client_sock, const std::string& ip, int port)

void handle_client(Socket& client_sock, const std::string& ip, int port)
{
    std::cout << "[+] 客户端连接: " << ip << ":" << port
              << " (fd=" << client_sock.Fd() << ")" << std::endl;

    char buf[BUF_SIZE];
    bool alive = true;

    while (alive)
    {
        std::memset(buf, 0, BUF_SIZE);
        ssize_t n = client_sock.Recv(buf, BUF_SIZE - 1);

        if (n < 0)
        {
            std::cerr << "[ERROR] recv() fd=" << client_sock.Fd() << ": " << std::strerror(errno) << std::endl;
            alive = false;
        }
        else if (n == 0)
        {
            // TCP 四次挥手：对端 close() → 我方 recv() 返回 0
            std::cout << "[-] 客户端断开: " << ip << ":" << port << " (fd=" << client_sock.Fd() << ")" << std::endl;
            alive = false;
        }
        else
        {
            std::string msg(buf, n);
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            {
                msg.pop_back();
            }
            std::cout << "[recv " << n << "B] " << msg << std::endl;

            ssize_t sent = client_sock.Send(buf, n);
            if (sent < 0)
            {
                std::cerr << "[ERROR] send() fd=" << client_sock.Fd() << ": " << std::strerror(errno) << std::endl;
                alive = false;
            }
        }
    }
    // client_sock 离开作用域 → 析构函数自动 close()
}


int main()
{
    try
    {
        // TODO: 1. 创建 Socket listen_sock
        Socket listen_sock;

        // TODO: 2. listen_sock.SetReuseAddr()
        listen_sock.SetReuseAddr();

        // TODO: 3. listen_sock.Bind(PORT)
        listen_sock.Bind(PORT);

        // TODO: 4. listen_sock.Listen(BACKLOG)
        listen_sock.Listen(BACKLOG);

        // TODO: 5. 打印启动信息
        std::cout << "\n========================================" << std::endl;
        std::cout << "  Echo Server 已启动 (阻塞模式)" << std::endl;
        std::cout << "  测试: nc localhost " << PORT << std::endl;
        std::cout << "  按 Ctrl+C 退出" << std::endl;
        std::cout << "========================================\n" << std::endl;

        // TODO: 6. while(true) 循环：
        //     std::string ip; int port;
        //     Socket client_sock = listen_sock.Accept(ip, port);
        //     handle_client(client_sock, ip, port);
        //     // client_sock 离开作用域自动 close
        while (true)
        {
            std::string client_ip;
            int client_port = 0;

            // accept() 阻塞直到有新连接
            Socket client_sock = listen_sock.Accept(client_ip, client_port);

            // 处理当前客户端（内部 recv/send 也是阻塞的）
            handle_client(client_sock, client_ip, client_port);

            // client_sock 析构 → close(fd)
            // 回到循环顶部，等待下一个客户端
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}