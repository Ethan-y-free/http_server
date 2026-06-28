/**
 * tcp_server.cpp —— 阻塞式 TCP Echo Server（RAII Socket 版）完整参考
 *
 * 对比阶段0 main.cpp：
 *   - fd 由 Socket 对象管理，不会忘 close
 *   - 错误全部走异常，不散落 errno 检查
 *   - try-catch 集中处理所有异常
 *
 * 编译：g++ -std=c++17 tcp_server.cpp -o tcp_server
 * 运行：./tcp_server
 * 测试：开另一个终端，nc localhost 8888
 */

#include <iostream>
#include <string>
#include <cstring>

#include "socket_raii.h"

namespace
{
    constexpr uint16_t PORT    = 8888;
    constexpr int      BACKLOG = 128;
    constexpr size_t   BUF_SIZE = 4096;
}


// ================================================================
// handle_client —— 处理单个客户端连接的 echo 循环
//
// 流程：循环 recv → 打印 → send 原样返回 → 直到客户端断开
//
// recv() 返回值：
//   > 0  : 实际读到的字节数
//   = 0  : 对端发送了 FIN，正常关闭连接
//   < 0  : 出错
//
// 阻塞式服务器的核心瓶颈就在这里：
//   recv() 不返回，什么也干不了——不能处理新连接，不能响应其他客户端
//   后面用 epoll 改非阻塞后，这个问题就解决了
// ================================================================
static void handle_client (Socket& client_sock, const std::string& ip, int port)
{
    std::cout << "[+] 客户端连接: " << ip << ":" << port
              << " (fd=" << client_sock.Fd () << ")" << std::endl;

    char buf[BUF_SIZE];
    bool alive = true;

    while (alive)
    {
        std::memset (buf, 0, BUF_SIZE);
        ssize_t n = client_sock.Recv (buf, BUF_SIZE - 1);

        if (n < 0)
        {
            std::cerr << "[ERROR] recv() fd=" << client_sock.Fd ()
                      << ": " << std::strerror (errno) << std::endl;
            alive = false;
        }
        else if (n == 0)
        {
            // TCP 四次挥手：对端 close() → 我方 recv() 返回 0
            std::cout << "[-] 客户端断开: " << ip << ":" << port
                      << " (fd=" << client_sock.Fd () << ")" << std::endl;
            alive = false;
        }
        else
        {
            // 去掉尾部换行，打印更整洁
            std::string msg (buf, n);
            while (!msg.empty () && (msg.back () == '\n' || msg.back () == '\r'))
            {
                msg.pop_back ();
            }
            std::cout << "[recv " << n << "B] " << msg << std::endl;

            // echo 原样返回
            ssize_t sent = client_sock.Send (buf, n);
            if (sent < 0)
            {
                std::cerr << "[ERROR] send() fd=" << client_sock.Fd ()
                          << ": " << std::strerror (errno) << std::endl;
                alive = false;
            }
        }
    }
    // client_sock 离开作用域 → 析构函数自动 close()
}


// ================================================================
// main —— 服务器主流程
//
// 五步走：
//   Socket → SetReuseAddr → Bind → Listen → while(Accept → handle)
//
// RAII 的好处在这里体现：
//   - try 块内正常执行完后，所有 Socket 对象自动 close
//   - 抛异常跳进 catch 时，栈展开也会触发析构
//   - 不用担心哪个分支忘了 close(fd)
// ================================================================
int main ()
{
    try
    {
        // ---- 1. 创建 TCP socket ----
        Socket listen_sock;
        std::cout << "[1/4] socket() 完成, fd=" << listen_sock.Fd () << std::endl;

        // ---- 2. 设置端口重用 ----
        listen_sock.SetReuseAddr ();
        std::cout << "[2/4] SO_REUSEADDR 已设置" << std::endl;

        // ---- 3. 绑定端口 ----
        listen_sock.Bind (PORT);
        std::cout << "[3/4] bind() → 0.0.0.0:" << PORT << std::endl;

        // ---- 4. 开始监听 ----
        listen_sock.Listen (BACKLOG);
        std::cout << "[4/4] listen() 完成" << std::endl;

        std::cout << "\n========================================" << std::endl;
        std::cout << "  Echo Server 已启动 (阻塞模式)" << std::endl;
        std::cout << "  测试: nc localhost " << PORT << std::endl;
        std::cout << "  按 Ctrl+C 退出" << std::endl;
        std::cout << "========================================\n" << std::endl;

        // ---- 5. 事件循环 ----
        // 阻塞式：一次只能服务一个客户端，当前客户端断开才能 accept 下一个
        while (true)
        {
            std::string client_ip;
            int client_port = 0;

            // accept() 阻塞直到有新连接
            Socket client_sock = listen_sock.Accept (client_ip, client_port);

            // 处理当前客户端（内部 recv/send 也是阻塞的）
            handle_client (client_sock, client_ip, client_port);

            // client_sock 析构 → close(fd)
            // 回到循环顶部，等待下一个客户端
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FATAL] " << e.what () << std::endl;
        return 1;
    }

    return 0;
}
