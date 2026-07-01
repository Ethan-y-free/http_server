#pragma once

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>

// ============================================================
// HttpRequest — HTTP 请求解析结果
// ============================================================

class HttpRequest
{
public:
    // ---------- Setters（由 HttpRequestParser 调用）----------

    void SetMethod(const std::string& m)  { method_ = m; }
    void SetPath(const std::string& p)    { path_ = p; }
    void SetVersion(const std::string& v) { version_ = v; }

    void AddHeader(const std::string& key, const std::string& value)
    {
        headers_[key] = value;
    }

    void AppendBody(const char* data, size_t len)
    {
        body_.append(data, len);
    }

    // ---------- Getters ----------

    const std::string& GetMethod()  const { return method_; }
    const std::string& GetPath()    const { return path_; }
    const std::string& GetVersion() const { return version_; }
    const std::string& GetBody()    const { return body_; }

    // 大小写不敏感查找 header
    std::string GetHeader(const std::string& key) const
    {
        for (const auto& kv : headers_)
        {
            if (StrCaseEqual(kv.first, key))
            {
                return kv.second;
            }
        }
        return {};
    }

    // ---------- 便利方法 ----------

    void Reset()
    {
        method_.clear();
        path_.clear();
        version_.clear();
        headers_.clear();
        body_.clear();
    }

    // HTTP/1.1 默认 keep-alive，除非 Connection: close
    // HTTP/1.0 默认 close，除非 Connection: keep-alive
    bool IsKeepAlive() const
    {
        std::string conn = GetHeader("Connection");
        std::string lower;
        for (char c : conn)
        {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (version_ == "HTTP/1.1")
        {
            return lower != "close";
        }
        return lower == "keep-alive";
    }

    size_t ContentLength() const
    {
        std::string val = GetHeader("Content-Length");
        if (val.empty())
        {
            return 0;
        }
        return static_cast<size_t>(std::stoull(val));
    }

private:
    static bool StrCaseEqual(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i]))
                != std::tolower(static_cast<unsigned char>(b[i])))
            {
                return false;
            }
        }
        return true;
    }

    std::string method_;    // "GET" / "POST" / "HEAD"
    std::string path_;      // "/index.html" (含 query string)
    std::string version_;   // "HTTP/1.1" / "HTTP/1.0"
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};
