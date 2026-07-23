#include "tcp_server.h"
#include "http_parser.h"
#include "http_static_handler.h"
#include "async_logger/logger.h"

AsyncLogWriter* g_logWriter = nullptr;

int main()
{
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
    server.Start();

    logWriter.Stop();
    return 0;
}