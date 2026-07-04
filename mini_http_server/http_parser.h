#pragma once

#include <string>
#include <algorithm>
#include <unordered_map>

#include "buffer.h"
#include "http_request.h"

class HttpRequestParser
{
public:
	enum State { PARSE_REQUEST_LINE, PARSE_HEADERS, PARSE_BODY, PARSE_DONE };
	enum ParseResult { PARSE_OK, PARSE_NEED_MORE, PARSE_ERROR };

	ParseResult Parse(Buffer* buffer)
	{
		ParseResult result = PARSE_OK;
		while (state_ != PARSE_DONE && result == PARSE_OK)
		{
			switch (state_)
			{
			case PARSE_REQUEST_LINE:
				result = ParseRequestLine(buffer);
				break;
			case PARSE_HEADERS:
				result = ParseHeaders(buffer);
				break;
			case PARSE_BODY:
				result = ParseBody(buffer);
				break;
			case PARSE_DONE:
				break;
			}
		}
		return result;
	}

	bool IsDone()              const { return state_ == PARSE_DONE; }
	State CurrentState()       const { return state_; }
	const HttpRequest& GetRequest() const { return request_; }

	void Reset()
	{
		state_ = PARSE_REQUEST_LINE;
		bodyRead_ = 0;
		request_.Reset();
	}

private:
	ParseResult ParseRequestLine(Buffer* buffer)
	{
		const char* crlf = FindCrLf(buffer->Peek(), buffer->ReadableBytes());
		if (!crlf)
		{
			return PARSE_NEED_MORE;
		}

		size_t lineLen = crlf - buffer->Peek();
		std::string line(buffer->Peek(), lineLen);

		size_t sp1 = line.find(' ');
		if (sp1 == std::string::npos)
		{
			return PARSE_ERROR;
		}

		size_t sp2 = line.find(' ', sp1 + 1);
		if (sp2 == std::string::npos)
		{
			return PARSE_ERROR;
		}

		if (line.find(' ', sp2 + 1) != std::string::npos)
		{
			return PARSE_ERROR;
		}

		std::string method = line.substr(0, sp1);
		std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);
		std::string ver = line.substr(sp2 + 1);

		// 4. 验证 method
		if (method != "GET" && method != "POST" && method != "HEAD")
		{
			return PARSE_ERROR;
		}

		// 5. 验证 path
		if (path.empty() || path[0] != '/')
		{
			return PARSE_ERROR;
		}

		// 6. 验证 version
		if (ver != "HTTP/1.1" && ver != "HTTP/1.0")
		{
			return PARSE_ERROR;
		}
		request_.SetMethod(std::move(method));
		request_.SetPath(std::move(path));
		request_.SetVersion(std::move(ver));
		buffer->Retrieve(lineLen + 2);

		state_ = PARSE_HEADERS;
		return PARSE_OK;
	}

	ParseResult ParseHeaders(Buffer* buffer)
	{
		while (true)
		{
			const char* crlf = FindCrLf(buffer->Peek(), buffer->ReadableBytes());
			if (!crlf)
			{
				return PARSE_NEED_MORE;
			}

			size_t lineLen = crlf - buffer->Peek();
			if (lineLen == 0)
			{
				buffer->Retrieve(2);
				if (request_.ContentLength() > 0)
				{
					state_ = PARSE_BODY;
				}
				else
				{
					state_ = PARSE_DONE;
				}
				return PARSE_OK;
			}

			std::string line(buffer->Peek(), lineLen);
			size_t colon = line.find(':');
			if (colon == std::string::npos)
			{
				return PARSE_ERROR;
			}

			std::string key = line.substr(0, colon);
			std::string value;
			size_t valStart = colon + 1;
			while (valStart < line.size() && line[valStart] == ' ')
			{
				++valStart;
			}
			value = line.substr(valStart);
			request_.AddHeader(std::move(key), std::move(value));
			buffer->Retrieve(lineLen + 2);
		}
	}

	ParseResult ParseBody(Buffer* buffer)
	{
		size_t contentLen = request_.ContentLength();
		size_t remain = contentLen - bodyRead_;
		size_t avail = buffer->ReadableBytes();

		if (avail >= remain)
		{
			request_.AppendBody(buffer->Peek(), remain);
			buffer->Retrieve(remain);
			bodyRead_ = contentLen;
			state_ = PARSE_DONE;
			return PARSE_OK;
		}
		else
		{
			request_.AppendBody(buffer->Peek(), avail);
			bodyRead_ += avail;
			buffer->Retrieve(avail);
			return PARSE_NEED_MORE;
		}
	}

	static const char* FindCrLf(const char* start, size_t len)
	{
		for (size_t i = 0; i + 1 < len; ++i)
		{
			if (start[i] == '\r' && start[i + 1] == '\n')
			{
				return start + i;
			}
		}
		return nullptr;
	}

	State state_ = PARSE_REQUEST_LINE;
	HttpRequest request_;
	size_t bodyRead_ = 0;
};