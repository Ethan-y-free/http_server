/**
 * epoll_echo_server_full.cpp —— epoll 多客户端并发 Echo Server 完整参考
 *
 * 目标：把阻塞式 server 升级为 epoll 驱动的非阻塞并发 server。
 *       一个线程就能同时服务几百个客户端。
 *
 * 对比阻塞式 server：
 *   - 阻塞式：while(1) { accept(卡住) → recv/send(卡住) } → 一次一个客户端
 *   - epoll 式：while(1) { epoll_wait(卡住) → 遍历就绪 fd } → 同时处理所有客户端
 *
 * 三个核心 API 精讲（面试必问）：
 *   epoll_create1()  → 创建 epoll 实例，返回一个 fd
 *   epoll_ctl()      → 向 epoll 实例"注册/修改/删除"要监控的 fd
 *   epoll_wait()     → 等待注册的 fd 中有事件发生
 *
 * 编译：g++ -std=c++17 epoll_echo_server.cpp -o epoll_echo_server
 * 运行：./epoll_echo_server
 * 测试：开 3 个终端各 nc localhost 8888，同时打字看回显
 */

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <unordered_map>

#include "socket_raii.h"
#include <sys/epoll.h>
#include <unistd.h>

namespace
{
    constexpr uint16_t PORT       = 8888;
    constexpr int      BACKLOG    = 128;
    constexpr int      MAX_EVENTS = 64;       // 一次 epoll_wait 最多返回的事件数
    constexpr size_t   BUF_SIZE   = 4096;
}


// ================================================================
// 主函数
// ================================================================
int main ()
{
    // ----------------------------------------------------------
    // 第 0 步：准备 listen socket（和阻塞版一样）
    // ----------------------------------------------------------
    Socket listen_sock;
    listen_sock.SetReuseAddr ();
    listen_sock.Bind (PORT);
    listen_sock.Listen (BACKLOG);

    // ----------------------------------------------------------
    // 第 1 步：设为非阻塞（epoll 的最佳实践）
    //
    // 为什么 listen fd 也要非阻塞？
    //   —— LT 模式下影响不大（只要还有连接，epoll 会持续通知）
    //   但 ET 模式下必须非阻塞，否则 accept 会阻塞整个事件循环
    //   现在养成设非阻塞的习惯，后面学 ET 时就顺理成章
    // ----------------------------------------------------------
    listen_sock.SetNonBlocking ();
    std::cout << "[1/5] listen fd=" << listen_sock.Fd ()
              << " 已设为非阻塞" << std::endl;

    // ----------------------------------------------------------
    // 第 2 步：创建 epoll 实例
    //
    // epoll_create1(flags)  —— 创建 epoll 实例，返回一个 fd
    //
    // 参数 flags：
    //   0               : 默认行为
    //   EPOLL_CLOEXEC   : close-on-exec，exec() 其他程序时自动关闭
    //                     防止子进程意外继承 epoll fd
    //
    // 返回值：
    //   >=0 : epoll 文件描述符（是的，epoll 本身也是个 fd！）
    //   -1  : 出错（errno 指示原因）
    //
    // 内核做了什么？
    //   —— 分配一个 eventpoll 结构体（红黑树 + 就绪链表）
    //   - 红黑树：存所有注册的 fd（快速查找/插入/删除 O(log n)）
    //   - 就绪链表：存有事件发生的 fd（epoll_wait 直接从这里取 O(1)）
    //
    // 面试考点：epoll_create vs epoll_create1？
    //   —— epoll_create(size) 的 size 是给内核的"提示"，2.6.8 起已废弃
    //   内核动态扩展，不需要预估大小
    //   epoll_create1 多了 EPOLL_CLOEXEC 支持
    // ----------------------------------------------------------
    int epfd = epoll_create1 (0);
    if (epfd < 0)
    {
        throw std::runtime_error (
            std::string ("epoll_create1() failed: ") + std::strerror (errno));
    }
    std::cout << "[2/5] epoll_create1() 完成, epfd=" << epfd << std::endl;

    // ----------------------------------------------------------
    // 第 3 步：将 listen fd 注册到 epoll
    //
    // epoll_ctl(epfd, op, fd, &event)  —— 向 epoll 实例注册/修改/删除一个 fd
    //
    // 参数详解：
    //   epfd    : epoll 实例的 fd（epoll_create1 的返回值）
    //   op      : 操作类型
    //     EPOLL_CTL_ADD  : 注册新的 fd → 插入内核红黑树
    //     EPOLL_CTL_MOD  : 修改已注册 fd 的监听事件类型
    //     EPOLL_CTL_DEL  : 从 epoll 移除 fd（fd 被 close 时内核自动 DEL）
    //   fd      : 要监听的 fd
    //   event   : struct epoll_event*，指定要监听什么
    //
    // struct epoll_event 结构（面试经常问！）：
    //   typedef union epoll_data {
    //       void    *ptr;   // 指向自定义结构体（最灵活，muduo 都用这个）
    //       int      fd;    // 存 fd 本身（简单场景够用）
    //       uint32_t u32;
    //       uint64_t u64;
    //   } epoll_data_t;
    //
    //   struct epoll_event {
    //       uint32_t     events;  // 监听哪些事件（位掩码 | 组合）
    //       epoll_data_t data;    // 用户自定义数据，事件触发时原样返回
    //   };
    //
    // events 位掩码详解：
    //   EPOLLIN      : fd 可读（有数据到达 or 新连接可 accept）
    //   EPOLLOUT     : fd 可写（发送缓冲区有空位）
    //   EPOLLET      : 边沿触发（ET），不加此标志默认电平触发（LT）
    //   EPOLLRDHUP   : 对端关闭连接或半关闭写端（Linux 2.6.17+）
    //   EPOLLPRI     : 有紧急数据（TCP 带外数据，很少用）
    //   EPOLLERR     : fd 发生错误（epoll 自动监听，不用手动设）
    //   EPOLLHUP     : fd 被挂断（epoll 自动监听，不用手动设）
    //   EPOLLONESHOT : 触发一次后自动禁用，需 EPOLL_CTL_MOD 重新激活
    //
    // 内核做了什么？
    //   —— 把 fd 插入 eventpoll 的红黑树
    //   并给这个 fd 注册一个内核回调：当 fd 就绪时，将其加入就绪链表
    // ----------------------------------------------------------
    struct epoll_event ev;
    std::memset (&ev, 0, sizeof (ev));
    ev.events = EPOLLIN;              // 监听可读 → 有新连接可 accept
    ev.data.fd = listen_sock.Fd ();   // 触发时通过 data.fd 找回是哪个 fd

    if (epoll_ctl (epfd, EPOLL_CTL_ADD, listen_sock.Fd (), &ev) < 0)
    {
        throw std::runtime_error (
            std::string ("epoll_ctl(ADD, listen_fd) failed: ") +
            std::strerror (errno));
    }
    std::cout << "[3/5] listen fd 已注册到 epoll" << std::endl;

    // ----------------------------------------------------------
    // 第 4 步：分配事件数组
    //
    // epoll_wait 返回时，内核把就绪事件填入这个数组
    // 数组大小 = 一次 epoll_wait 最多返回的事件数
    //
    // MAX_EVENTS 设多大合适？
    //   - 太小：高并发时一次取不完，要多次 epoll_wait（多一次系统调用开销）
    //   - 太大：占用内存（每个 epoll_event 才 12 字节，1000 个也才 12KB）
    //   - 通常设 128~1024，够用不浪费
    // ----------------------------------------------------------
    std::vector<struct epoll_event> ready_events (MAX_EVENTS);

    // 存放所有客户端 Socket 对象，保证 fd 生命周期
    // key=fd, value=Socket —— Socket 析构自动 close(fd)
    std::unordered_map<int, Socket> clients;

    std::cout << "[4/5] 事件数组就绪, MAX_EVENTS=" << MAX_EVENTS << std::endl;

    // ----------------------------------------------------------
    // 第 5 步：事件循环 —— 整个服务器就这一个循环
    //
    // epoll_wait(epfd, events, maxevents, timeout)
    //
    // 参数：
    //   epfd      : epoll 实例 fd
    //   events    : 输出参数，内核把就绪事件写到这里
    //   maxevents : events 数组大小，一次最多返回这么多
    //   timeout   : 超时时间（毫秒）
    //     -1  : 永远等待，直到有事件（最常用，让 CPU 休眠）
    //      0  : 非阻塞轮询，立刻返回（忙等，CPU 100%，极少用）
    //     >0  : 等待 N 毫秒，超时返回 0（有定时任务时用）
    //
    // 返回值：
    //   >0 : 就绪的 fd 数量 ← events[0..nfds-1] 中填了数据
    //   =0 : 超时（只有 timeout >= 0 才会发生）
    //   -1 : 出错（errno == EINTR 表示被信号中断，重试即可）
    //
    // ★ 内核执行流程（面试高频题！）：
    //   1. epoll_wait 调用让当前线程睡眠
    //   2. 当某 fd 有数据到达 → 网卡硬中断 → 内核协议栈处理
    //      → 调用 ep_poll_callback 回调（在注册时设置好的）
    //   3. ep_poll_callback 把该 fd 的 epoll_event 加入就绪链表
    //   4. 唤醒 epoll_wait 上睡眠的线程
    //   5. epoll_wait 把就绪链表的条目拷贝到用户提供的 events 数组
    //   6. 返回就绪数量
    //
    // ★ epoll vs select/poll（面试必问！）：
    //
    //   select/poll 的问题：
    //     ① 每次调用都要把"全部监听 fd 集合"从用户态拷贝到内核 O(n)
    //     ② 内核遍历全部 fd 检查哪些就绪 O(n)
    //     ③ select 有 FD_SETSIZE=1024 硬上限
    //
    //   epoll 的改进：
    //     ① fd 只注册一次（epoll_ctl），存在内核红黑树中 O(log n)
    //     ② 就绪通知由内核回调驱动，不用遍历 O(1)
    //     ③ epoll_wait 只拷贝就绪 fd，不是全部 fd O(ready_count)
    //     ④ 没有 fd 数量上限
    //
    //   LT vs ET（面试必问！）：
    //     LT（Level Triggered，电平触发，默认）：
    //       fd 就绪时 → epoll_wait 返回此 fd
    //       如果你没处理完（没读到 EAGAIN），下次 epoll_wait 还会返回它
    //       类似：只要水位高于阈值就一直报警
    //       优点：不容易丢事件，编程简单
    //       缺点：可能重复通知，多做无用功
    //
    //     ET（Edge Triggered，边沿触发，加 EPOLLET）：
    //       fd 从"不就绪→就绪"时 → epoll_wait 返回此 fd（只通知一次！）
    //       你必须循环 read/write 直到 EAGAIN，否则那部分数据就丢了
    //       类似：只在水位越过阈值的瞬间报警一次
    //       优点：减少重复通知，高并发下效率更高
    //       缺点：编程复杂，必须非阻塞 IO + 循环读完
    // ----------------------------------------------------------
    std::cout << "[5/5] 进入事件循环\n" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Epoll Echo Server 已启动 (LT 模式)" << std::endl;
    std::cout << "  测试: nc localhost " << PORT << std::endl;
    std::cout << "  开多个终端同时测试!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    while (true)
    {
        // ---- 等待事件 ----
        // -1 = 一直等到有事件，不让 CPU 空转
        int nfds = epoll_wait (epfd, ready_events.data (), MAX_EVENTS, -1);
        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                // Ctrl+C 等信号会导致 EINTR，继续循环即可
                continue;
            }
            std::cerr << "[ERROR] epoll_wait() failed: "
                      << std::strerror (errno) << std::endl;
            break;
        }

        // ---- 遍历所有就绪 fd ----
        for (int i = 0; i < nfds; ++i)
        {
            int ready_fd = ready_events[i].data.fd;
            uint32_t revents = ready_events[i].events;

            // =====================================================
            // 情况 A：listen fd 就绪 → 有新连接
            // =====================================================
            if (ready_fd == listen_sock.Fd ())
            {
                // LT 模式：只要 accept 队列不空，epoll 会持续通知
                // 所以每次只 accept 一次即可，下次 epoll_wait 还会触发
                // （如果要一次 accept 完，需要用 while + 处理 EAGAIN）
                std::string client_ip;
                int client_port = 0;
                Socket client_sock = listen_sock.Accept (client_ip, client_port);

                std::cout << "[+] 新客户端: " << client_ip << ":" << client_port
                          << " (fd=" << client_sock.Fd () << ")" << std::endl;

                // 客户端 fd 也设为非阻塞
                client_sock.SetNonBlocking ();

                // 把客户端 fd 注册到 epoll，监听可读 + 对端关闭
                struct epoll_event client_ev;
                std::memset (&client_ev, 0, sizeof (client_ev));
                client_ev.events = EPOLLIN | EPOLLRDHUP;
                client_ev.data.fd = client_sock.Fd ();

                if (epoll_ctl (epfd, EPOLL_CTL_ADD, client_sock.Fd (), &client_ev) < 0)
                {
                    std::cerr << "[ERROR] epoll_ctl(ADD) fd="
                              << client_sock.Fd () << " failed: "
                              << std::strerror (errno) << std::endl;
                    continue;  // client_sock 析构自动 close
                }

                // ★ 把 Socket 移入 clients 容器，延长生命周期 ★
                // client_sock 是局部变量，离开作用域会析构 → close(fd)
                // 移入 map 后，fd 的生命周期由 map 管理
                // 客户端断开时从 map 移除 → 析构 → close(fd)
                int client_fd = client_sock.Fd ();
                clients.emplace (client_fd, std::move (client_sock));
            }
            // =====================================================
            // 情况 B：客户端异常断开（错误/挂断/对端关闭）
            //
            // 检查顺序：先检查错误，再检查可读
            // 因为 EPOLLERR/EPOLLHUP 和 EPOLLIN 可能同时出现
            // 如果先处理可读，recv 会收到错误
            // =====================================================
            else if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            {
                std::cout << "[-] 客户端断开 (fd=" << ready_fd
                          << ", events=" << revents << ")" << std::endl;
                epoll_ctl (epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                clients.erase (ready_fd);  // erase → Socket 析构 → close(fd)
            }
            // =====================================================
            // 情况 C：客户端发来数据 → echo 回去
            // =====================================================
            else if (revents & EPOLLIN)
            {
                char buf[BUF_SIZE];
                std::memset (buf, 0, BUF_SIZE);
                ssize_t n = recv (ready_fd, buf, BUF_SIZE - 1, 0);

                if (n < 0)
                {
                    // 非阻塞 fd：没数据时返回 EAGAIN，不是真错
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        continue;
                    }
                    std::cerr << "[ERROR] recv() fd=" << ready_fd
                              << ": " << std::strerror (errno) << std::endl;
                    epoll_ctl (epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                    clients.erase (ready_fd);
                }
                else if (n == 0)
                {
                    // TCP 正常关闭：对端发了 FIN，recv 返回 0
                    std::cout << "[-] 客户端关闭连接 (fd=" << ready_fd << ")" << std::endl;
                    epoll_ctl (epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                    clients.erase (ready_fd);
                }
                else
                {
                    // 收到数据 → 打印 + 回显
                    std::string msg (buf, n);
                    while (!msg.empty () && (msg.back () == '\n' || msg.back () == '\r'))
                    {
                        msg.pop_back ();
                    }
                    std::cout << "[recv " << n << "B fd=" << ready_fd << "] "
                              << msg << std::endl;

                    ssize_t sent = send (ready_fd, buf, n, 0);
                    if (sent < 0)
                    {
                        std::cerr << "[ERROR] send() fd=" << ready_fd
                                  << ": " << std::strerror (errno) << std::endl;
                        epoll_ctl (epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                        clients.erase (ready_fd);
                    }
                }
            }
        }
    }

    // 清理：close epoll fd
    close (epfd);
    return 0;
}
