#include "tcp_server.h"
#include "http_parser.h"
#include "http_static_handler.h"
#include "async_logger/logger.h"

#include <csignal>
#include <thread>

AsyncLogWriter* g_logWriter = nullptr;

// 全局指针，供信号处理线程触发优雅关闭
static TcpServer* g_server = nullptr;

int main()
{
    // ★ 必须在所有线程创建之前阻塞信号，否则子线程不继承阻塞掩码
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    AsyncLogWriter logWriter("server.log");
    g_logWriter = &logWriter;
    logWriter.Start();

    HttpStaticHandler handler("www");
    auto onMessage = [&handler](TcpConnection* conn, Buffer* input)
        {
            HttpRequestParser parser;
            while (input->ReadableBytes() > 0)
            {
                auto result = parser.Parse(input);
                if (result == HttpRequestParser::PARSE_NEED_MORE) break;
                if (result == HttpRequestParser::PARSE_ERROR) { conn->ForceClose(); return; }

                const HttpRequest& req = parser.GetRequest();
                Buffer response;
                handler.HandleRequest(req, &response);
                conn->Send(response.Peek(), response.ReadableBytes());

                if (!req.IsKeepAlive()) { conn->Shutdown(); return; }
                parser.Reset();
            }
        };


    TcpServerConfig config;
    config.port = 8888;
    config.subReactorCount = 4;
    config.idleTimeoutMs = 60000;
    config.onMessage = onMessage;

    TcpServer server(config, &logWriter);
    g_server = &server;

    // 用独立线程 sigwait 等待被阻塞的信号，收到后优雅关闭
    std::thread signalThread([&set]()
        {
            int sig = 0;
            sigwait(&set, &sig);
            if (g_server) g_server->Quit();
        });

    server.Start();

    signalThread.join();
    logWriter.Stop();
    return 0;
}