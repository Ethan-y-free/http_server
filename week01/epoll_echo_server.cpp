/**
 * epoll_echo_server.cpp —— epoll 多客户端并发 Echo Server（LT 默认模式）
 *
 * 目标：用 epoll 把阻塞式 server 升级为非阻塞并发 server。
 *       一个线程同时服务多个客户端。
 *
 * 你需要实现的三个核心 API：
 *   ① epoll_create1(0) → 创建 epoll 实例
 *   ② epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) → 注册 fd 到 epoll
 *   ③ epoll_wait(epfd, events, MAX_EVENTS, -1) → 等待事件发生
 *
 * 编译：g++ -std=c++17 epoll_echo_server.cpp -o epoll_echo_server
 * 运行：./epoll_echo_server
 * 测试：开 3 个终端各 nc localhost 8888，同时打字看回显
 */

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <cstring>
#include <unordered_map>

#include "socket_raii.h"
#include <sys/epoll.h>
#include <unistd.h>

// TODO: 定义常量 PORT=8888, BACKLOG=128, MAX_EVENTS=64, BUF_SIZE=4096
constexpr uint16_t PORT = 8888;
constexpr int      BACKLOG = 128;
constexpr int      MAX_EVENTS = 64;       // 一次 epoll_wait 最多返回的事件数
constexpr size_t   BUF_SIZE = 4096;


int main ()
{
    // ================================================================
    // 第 1 步：创建 listen socket + bind + listen（你已经会了）
    // ================================================================
    // TODO: Socket listen_sock → SetReuseAddr → Bind(PORT) → Listen(BACKLOG)
    // TODO: listen_sock.SetNonBlocking() —— epoll 模式下 fd 通常设为非阻塞
    Socket listen_sock;
    listen_sock.SetReuseAddr();
    listen_sock.Bind(PORT);
    listen_sock.Listen(BACKLOG);
    listen_sock.SetNonBlocking();

    // ================================================================
    // 第 2 步：创建 epoll 实例
    //
    // int epoll_create1(int flags);
    //   参数：0 或 EPOLL_CLOEXEC（exec 时自动关闭）
    //   返回：epoll 文件描述符（是的，epoll 本身也是一个 fd！）
    //   失败：返回 -1
    // ================================================================
    // TODO: int epfd = epoll_create1(0);
    // TODO: 检查返回值，失败抛异常
    int epfd = epoll_create1(0);
    if (epfd < 0)
    {
        throw std::runtime_error(std::string("epoll_create1() failed: ") + std::strerror(errno));
    }

    // ================================================================
    // 第 3 步：把 listen fd 注册到 epoll
    //
    // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    //
    //   epfd   : epoll 实例 fd
    //   op     : EPOLL_CTL_ADD（注册）/ EPOLL_CTL_MOD（修改）/ EPOLL_CTL_DEL（删除）
    //   fd     : 要监听的 fd
    //   event  : 指定监听什么事件 + 携带的用户数据
    //
    // struct epoll_event {
    //     uint32_t     events;   // 位掩码：EPOLLIN | EPOLLOUT | EPOLLET | ...
    //     epoll_data_t data;     // union: data.fd 或 data.ptr
    // };
    //
    // 常用 events：
    //   EPOLLIN    : 可读（新连接 or 新数据）
    //   EPOLLOUT   : 可写
    //   EPOLLRDHUP : 对端关闭连接
    //   EPOLLET    : 边沿触发（不加默认 LT 电平触发）
    //   EPOLLERR / EPOLLHUP : epoll 自动监听，不用手动加
    // ================================================================
    // TODO: struct epoll_event ev;
    // TODO: ev.events = EPOLLIN;
    // TODO: ev.data.fd = listen_sock.Fd();
    // TODO: epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock.Fd(), &ev);
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock.Fd();
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock.Fd(), &ev) < 0)
    {
        throw std::runtime_error(std::string("epoll_ctl(ADD, listen_fd) failed: ") + std::strerror(errno));
    }

    // ================================================================
    // 第 4 步：准备事件数组 + 客户端容器
    //
    // ready_events：epoll_wait 返回时内核把就绪事件写到这里
    // clients：存所有客户端 Socket，防止 fd 被提前析构
    // ================================================================
    // TODO: std::vector<struct epoll_event> ready_events(MAX_EVENTS);
    // TODO: std::unordered_map<int, Socket> clients;
    std::vector<epoll_event> ready_events(MAX_EVENTS);
    std::unordered_map<int, Socket> clients;
     
    // ================================================================
    // 第 5 步：事件循环
    //
    // int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
    //
    //   epfd      : epoll 实例
    //   events    : 输出参数，就绪事件写到这里
    //   maxevents : 数组大小
    //   timeout   : -1=永远等, 0=立刻返回, >0=等N毫秒
    //   返回      : 就绪 fd 数量, 0=超时, -1=错误
    //
    // 循环结构：
    //   while (true) {
    //       nfds = epoll_wait(epfd, ...);
    //       for (i = 0..nfds-1) {
    //           ready_fd = ready_events[i].data.fd;
    //           revents  = ready_events[i].events;
    //
    //           if (ready_fd == listen_fd) {
    //               → accept 新连接 → 注册到 epoll → 存入 clients 容器
    //           }
    //           else if (revents & (EPOLLERR|EPOLLHUP|EPOLLRDHUP)) {
    //               → 客户端断开 → epoll_ctl DEL → clients.erase
    //           }
    //           else if (revents & EPOLLIN) {
    //               → recv 数据 → 打印 → send 回显
    //               → recv 返回 0 = 对端关闭
    //               → recv 返回 <0 && errno==EAGAIN = 非阻塞没数据（正常）
    //           }
    //       }
    //   }
    // ================================================================
    // TODO: 实现上述事件循环
    while (true)
    {
        int nfds = epoll_wait(epfd, ready_events.data(), MAX_EVENTS, -1);
        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                // Ctrl+C 等信号会导致 EINTR，继续循环即可
                continue;
            }
            std::cerr << "[ERROR] epoll_wait() failed: " << std::strerror(errno) << std::endl;
            break;
        }
        for (int i = 0; i < nfds; i++)
        {
            int ready_fd = ready_events[i].data.fd;
            uint32_t revents = ready_events[i].events;

            if (ready_fd == listen_sock.Fd())
            {
                std::string client_ip;
                int client_port = 0;
                Socket client_sock = listen_sock.Accept(client_ip, client_port);
                if (client_sock.Fd() < 0) continue;
                std::cout << "[+] 新客户端: " << client_ip << ":" << client_port << " (fd=" << client_sock.Fd() << ")" << std::endl;

                client_sock.SetNonBlocking();

                epoll_event client_ev;
                std::memset(&client_ev, 0, sizeof(client_ev));
                client_ev.events = EPOLLIN | EPOLLRDHUP;
                client_ev.data.fd = client_sock.Fd();

                if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_sock.Fd(), &client_ev) < 0)
                {
                    std::cerr << "[ERROR] epoll_ctl(ADD) fd=" << client_sock.Fd() << " failed: " << std::strerror(errno) << std::endl;
                    continue;  // client_sock 析构自动 close
                }

                // ★ 把 Socket 移入 clients 容器，延长生命周期 ★
                // client_sock 是局部变量，离开作用域会析构 → close(fd)
                // 移入 map 后，fd 的生命周期由 map 管理
                // 客户端断开时从 map 移除 → 析构 → close(fd)
                int client_fd = client_sock.Fd();
                clients.emplace(client_fd, std::move(client_sock));
            }
            else if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            {
                std::cout << "[-] 客户端断开 (fd=" << ready_fd << ", events=" << revents << ")" << std::endl;
                epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                clients.erase(ready_fd);  // erase → Socket 析构 → close(fd)
            }
            else if (revents & EPOLLIN)
            {
                char buf[BUF_SIZE];
                std::memset(buf, 0, BUF_SIZE);
                ssize_t n = recv(ready_fd, buf, BUF_SIZE - 1, 0);

                if (n < 0)
                {
                    // 非阻塞 fd：没数据时返回 EAGAIN，不是真错
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        continue;
                    }
                    std::cerr << "[ERROR] recv() fd=" << ready_fd << ": " << std::strerror(errno) << std::endl;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                    clients.erase(ready_fd);
                }
                else if (n == 0)
                {
                    // TCP 正常关闭：对端发了 FIN，recv 返回 0
                    std::cout << "[-] 客户端关闭连接 (fd=" << ready_fd << ")" << std::endl;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                    clients.erase(ready_fd);
                }
                else
                {
                    std::string msg(buf, n);
                    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                    {
                        msg.pop_back();
                    }

                    std::cout << "[recv " << n << "B fd=" << ready_fd << "] " << msg << std::endl;

                    ssize_t sent = send(ready_fd, buf, n, 0);
                    if (sent < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            // 缓冲区满，下次 epoll EPOLLOUT 时再发，这里简单跳过
                            continue;
                        }
                        std::cerr << "[ERROR] send() fd=" << ready_fd << ": " << std::strerror(errno) << std::endl;
                        epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                        clients.erase(ready_fd);
                    }
                }
            }
        }
    }


    // 清理
    // TODO: close(epfd);
    close(epfd);
    return 0;
}
