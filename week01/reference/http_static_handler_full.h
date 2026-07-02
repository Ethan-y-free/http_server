#pragma once

#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cstring>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>

#include "../buffer.h"
#include "../http_request.h"

// ============================================================
// HttpStaticHandler — 静态文件处理器
//
// 职责：
//   1. URL 路径 → 文件系统路径映射（含目录穿越防护）
//   2. MIME 类型检测（按文件扩展名）
//   3. 读文件 + 生成 HTTP 响应
//
// 用法：
//   HttpStaticHandler handler("./www");
//   handler.SetIndexFile("index.html");
//   handler.HandleRequest(req, &outputBuffer);
// ============================================================

class HttpStaticHandler
{
public:
    // ----------------------------------------------------------
    // 构造函数
    // root_dir: 静态文件根目录（可以是相对路径或绝对路径）
    // ----------------------------------------------------------
    explicit HttpStaticHandler(const std::string& root_dir)
        : root_dir_(root_dir)
        , index_file_("index.html")
    {
        // 去掉 root_dir 末尾的 '/'
        while (!root_dir_.empty() && root_dir_.back() == '/')
        {
            root_dir_.pop_back();
        }
    }

    // ----------------------------------------------------------
    // 设置索引文件名（请求目录时默认返回的文件）
    // ----------------------------------------------------------
    void SetIndexFile(const std::string& index)
    {
        index_file_ = index;
    }

    // ----------------------------------------------------------
    // 主入口：根据 HttpRequest 生成 HTTP 响应，写入 output buffer
    // ----------------------------------------------------------
    void HandleRequest(const HttpRequest& req, Buffer* output)
    {
        const std::string& method = req.GetMethod();

        // 只支持 GET 和 HEAD
        if (method != "GET" && method != "HEAD")
        {
            BuildResponse(405, "Method Not Allowed",
                          "<html><body><h1>405 Method Not Allowed</h1></body></html>",
                          "text/html; charset=utf-8",
                          req.IsKeepAlive(), req.GetVersion(),
                          req.GetMethod() == "HEAD", output);
            return;
        }

        // 1. URL 解码
        std::string url_path = UrlDecode(req.GetPath());

        // 2. 路径映射 + 安全检查
        std::string file_path = MapPath(url_path);
        if (file_path.empty())
        {
            BuildResponse(403, "Forbidden",
                          "<html><body><h1>403 Forbidden</h1></body></html>",
                          "text/html; charset=utf-8",
                          req.IsKeepAlive(), req.GetVersion(),
                          false, output);
            return;
        }

        // 3. 如果是目录，尝试索引文件
        if (IsDirectory(file_path))
        {
            std::string index_path = file_path;
            if (index_path.back() != '/')
            {
                index_path += '/';
            }
            index_path += index_file_;

            // 索引文件存在就用它，不存在返回 403（禁止列目录）
            if (FileExists(index_path))
            {
                file_path = index_path;
            }
            else
            {
                BuildResponse(403, "Forbidden",
                              "<html><body><h1>403 Forbidden</h1>"
                              "<p>Directory listing not allowed.</p></body></html>",
                              "text/html; charset=utf-8",
                              req.IsKeepAlive(), req.GetVersion(),
                              false, output);
                return;
            }
        }

        // 4. 读文件
        std::string content;
        if (!ReadFile(file_path, content))
        {
            // 区分文件不存在 vs 读取失败
            if (!FileExists(file_path))
            {
                BuildResponse(404, "Not Found",
                              "<html><body><h1>404 Not Found</h1>"
                              "<p>The requested URL " + EscapeHtml(req.GetPath()) +
                              " was not found on this server.</p></body></html>",
                              "text/html; charset=utf-8",
                              req.IsKeepAlive(), req.GetVersion(),
                              false, output);
            }
            else
            {
                BuildResponse(500, "Internal Server Error",
                              "<html><body><h1>500 Internal Server Error</h1></body></html>",
                              "text/html; charset=utf-8",
                              false, req.GetVersion(),  // 出错就关连接
                              false, output);
            }
            return;
        }

        // 5. 成功 — 200 OK
        std::string mime = GetMimeType(file_path);
        BuildResponse(200, "OK", content, mime,
                      req.IsKeepAlive(), req.GetVersion(),
                      req.GetMethod() == "HEAD", output);
    }

private:
    // ----------------------------------------------------------
    // URL 解码：%20 → 空格，%2F → / 等
    // 大小写不敏感
    // ----------------------------------------------------------
    static std::string UrlDecode(const std::string& str)
    {
        std::string result;
        result.reserve(str.size());

        for (size_t i = 0; i < str.size(); ++i)
        {
            if (str[i] == '%' && i + 2 < str.size()
                && IsHex(str[i + 1]) && IsHex(str[i + 2]))
            {
                char decoded = static_cast<char>(HexToInt(str[i + 1]) * 16
                                               + HexToInt(str[i + 2]));
                result += decoded;
                i += 2;
            }
            else if (str[i] == '+')
            {
                // query string 中 + 表示空格（path 中少见但保留）
                result += ' ';
            }
            else
            {
                result += str[i];
            }
        }

        return result;
    }

    // ----------------------------------------------------------
    // URL 路径 → 文件系统绝对路径
    // 返回空串表示拒绝访问（检测到 .. 目录穿越）
    // ----------------------------------------------------------
    std::string MapPath(const std::string& url_path)
    {
        // 安全检查：禁止 ".." 路径穿越
        // 注意：UrlDecode 已经在此之前调用，编码绕过已被解除
        if (url_path.find("..") != std::string::npos)
        {
            return "";  // 拒绝
        }

        // 拼接根目录
        std::string result = root_dir_ + url_path;

        // 规范化：去掉连续的 //
        for (size_t i = 0; i + 1 < result.size(); )
        {
            if (result[i] == '/' && result[i + 1] == '/')
            {
                result.erase(i, 1);
                // i 不变，继续检查同一位置
            }
            else
            {
                ++i;
            }
        }

        return result;
    }

    // ----------------------------------------------------------
    // MIME 类型检测：根据文件扩展名
    // ----------------------------------------------------------
    static std::string GetMimeType(const std::string& path)
    {
        static const std::unordered_map<std::string, std::string> kMimeTypes =
        {
            // 文本类
            { ".html",    "text/html; charset=utf-8" },
            { ".htm",     "text/html; charset=utf-8" },
            { ".css",     "text/css; charset=utf-8" },
            { ".js",      "application/javascript; charset=utf-8" },
            { ".mjs",     "application/javascript; charset=utf-8" },
            { ".json",    "application/json; charset=utf-8" },
            { ".xml",     "application/xml; charset=utf-8" },
            { ".txt",     "text/plain; charset=utf-8" },
            { ".csv",     "text/csv; charset=utf-8" },
            { ".md",      "text/markdown; charset=utf-8" },

            // 图片类
            { ".png",     "image/png" },
            { ".jpg",     "image/jpeg" },
            { ".jpeg",    "image/jpeg" },
            { ".gif",     "image/gif" },
            { ".svg",     "image/svg+xml" },
            { ".ico",     "image/x-icon" },
            { ".webp",    "image/webp" },
            { ".bmp",     "image/bmp" },

            // 字体类
            { ".woff",    "font/woff" },
            { ".woff2",   "font/woff2" },
            { ".ttf",     "font/ttf" },
            { ".eot",     "application/vnd.ms-fontobject" },
            { ".otf",     "font/otf" },

            // 音视频类
            { ".mp3",     "audio/mpeg" },
            { ".mp4",     "video/mp4" },
            { ".webm",    "video/webm" },
            { ".ogg",     "audio/ogg" },
            { ".wav",     "audio/wav" },

            // 文档类
            { ".pdf",     "application/pdf" },
            { ".zip",     "application/zip" },
            { ".gz",      "application/gzip" },
            { ".tar",     "application/x-tar" },

            // 源码类
            { ".cpp",     "text/plain; charset=utf-8" },
            { ".h",       "text/plain; charset=utf-8" },
            { ".py",      "text/plain; charset=utf-8" },
            { ".java",    "text/plain; charset=utf-8" },
        };

        // 找最后一个 '.' 取扩展名
        size_t dot = path.rfind('.');
        if (dot == std::string::npos)
        {
            return "application/octet-stream";
        }

        std::string ext = path.substr(dot);
        // 转小写
        for (auto& c : ext)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        auto it = kMimeTypes.find(ext);
        if (it != kMimeTypes.end())
        {
            return it->second;
        }

        return "application/octet-stream";
    }

    // ----------------------------------------------------------
    // 读文件：二进制模式，整个文件读到 string
    // 返回 true 表示成功
    // ----------------------------------------------------------
    static bool ReadFile(const std::string& path, std::string& content)
    {
        // 二进制模式：防止 Windows 上 \r\n 转换损坏图片等二进制文件
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            return false;
        }

        // ate 模式下 tellg 返回文件大小
        std::streamsize size = file.tellg();
        if (size < 0)
        {
            return false;
        }

        file.seekg(0, std::ios::beg);

        content.resize(static_cast<size_t>(size));
        if (size > 0)
        {
            file.read(&content[0], size);
        }

        return file.good() || file.eof();
        // eof 也算成功（读完最后一行后 eofbit 会置位）
    }

    // ----------------------------------------------------------
    // 构建完整 HTTP 响应报文，写入 Buffer
    // ----------------------------------------------------------
    static void BuildResponse(int status_code,
                              const std::string& status_msg,
                              const std::string& body,
                              const std::string& mime_type,
                              bool keep_alive,
                              const std::string& version,
                              bool head_only,
                              Buffer* output)
    {
        // 用 stringstream 拼接响应头和响应体
        // 注意：对于 HEAD 请求，Content-Length 写实际大小但 body 不发送

        size_t body_size = body.size();
        const std::string& actual_body = head_only ? kEmptyBody_ : body;

        // 手动拼接以精确控制格式
        char header_buf[4096];
        int header_len = snprintf(header_buf, sizeof(header_buf),
            "%s %d %s\r\n"
            "Server: tiny-http/1.0\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "\r\n",
            version.c_str(),
            status_code,
            status_msg.c_str(),
            mime_type.c_str(),
            body_size,
            keep_alive ? "keep-alive" : "close");

        output->Append(header_buf, static_cast<size_t>(header_len));

        if (!actual_body.empty())
        {
            output->Append(actual_body.data(), actual_body.size());
        }
    }

    // ----------------------------------------------------------
    // 工具函数
    // ----------------------------------------------------------

    static bool IsHex(char c)
    {
        return (c >= '0' && c <= '9')
            || (c >= 'A' && c <= 'F')
            || (c >= 'a' && c <= 'f');
    }

    static int HexToInt(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }

    static bool IsDirectory(const std::string& path)
    {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0)
        {
            return false;
        }
        return S_ISDIR(st.st_mode);
    }

    static bool FileExists(const std::string& path)
    {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }

    // HTML 实体转义（防止 XSS）
    static std::string EscapeHtml(const std::string& str)
    {
        std::string result;
        result.reserve(str.size());
        for (char c : str)
        {
            switch (c)
            {
            case '<':  result += "&lt;";   break;
            case '>':  result += "&gt;";   break;
            case '&':  result += "&amp;";  break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&#39;";  break;
            default:   result += c;        break;
            }
        }
        return result;
    }

    // ----------------------------------------------------------
    // 成员变量
    // ----------------------------------------------------------

    std::string root_dir_;
    std::string index_file_;

    // 用于 HEAD 请求的空 body（避免构造临时空 string）
    static const std::string kEmptyBody_;
};

// 静态成员定义
const std::string HttpStaticHandler::kEmptyBody_;
