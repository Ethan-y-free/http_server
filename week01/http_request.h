#pragma once

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>

class HttpRequest
{
public:
	void SetMethod(const std::string& m) { method_ = m; }
	void SetPath(const std::string& p) { path_ = p; }
	void SetVersion(const std::string& v) { version_ = v; }

	// ---------- Getters ----------

	const std::string& GetMethod()  const { return method_; }
	const std::string& GetPath()    const { return path_; }
	const std::string& GetVersion() const { return version_; }
	const std::string& GetBody()    const { return body_; }

	void AddHeader(const std::string& key, const std::string& value)
	{
		headers_[key] = value;
	}

	void AppendBody(const char* data, size_t len)
	{
		body_.append(data, len);
	}

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

	bool IsKeepAlive() const
	{
		std::string conn = GetHeader("Connection");
		// 值大小写不敏感比较（ab 发 "Keep-Alive"，curl 发 "keep-alive"）
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

	void Reset()
	{
		method_.clear();
		path_.clear();
		version_.clear();
		headers_.clear();
		body_.clear();
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
			if (std::tolower(static_cast<unsigned char>(a[i])) !=
				std::tolower(static_cast<unsigned char>(b[i])))
			{
				return false;
			}
		}
		return true;
	}

	std::string method_;
	std::string path_;
	std::string version_;
	std::unordered_map<std::string, std::string> headers_;
	std::string body_;
};