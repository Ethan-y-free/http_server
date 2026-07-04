#pragma once

#include "log_stream.h"
#include "log_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <sys/time.h>

enum class LogLevel { TRACE, DEBUG, INFO, WARN, ERROR, FATAL };

class Logger
{
public:
	Logger(const char* file, int line, LogLevel level) : file_(file), line_(line), level_(level)
	{
		// 格式化时间戳前缀：[2026-07-04 14:30:12.123456] [INFO ] file.cpp:239
		struct timeval tv;
		gettimeofday(&tv, nullptr);

		struct tm tm_time;
		localtime_r(&tv.tv_sec, &tm_time);

		char timeBuf[32];
		int timeLen = snprintf(timeBuf, sizeof(timeBuf),
			"%04d-%02d-%02d %02d:%02d:%02d.%06ld",
			tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
			tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
			static_cast<long>(tv.tv_usec));
		stream_ << std::string(timeBuf, timeLen);

		// 日志级别
		const char* levelStr = "UNKNOWN";
		switch (level_)
		{
		case LogLevel::TRACE: levelStr = "TRACE "; break;
		case LogLevel::DEBUG: levelStr = "DEBUG "; break;
		case LogLevel::INFO:  levelStr = "INFO  "; break;
		case LogLevel::WARN:  levelStr = "WARN  "; break;
		case LogLevel::ERROR: levelStr = "ERROR "; break;
		case LogLevel::FATAL: levelStr = "FATAL "; break;
		}
		stream_ << '[' << levelStr << "] ";

		// 文件名:行号
		stream_ << file_ << ':' << line_ << ' ';
	}

	~Logger()
	{
		stream_ << '\n';                               // 1. 换行收尾

		LogBuffer* buf = CurrentLogBuffer();           // 2. 找到当前线程的 buffer
		if (buf)
		{
			buf->Append(stream_.Data(), stream_.Size());  // 3. 提交到双缓冲
		}

		if (level_ == LogLevel::FATAL)                 // 4. FATAL 特殊处理
		{
			fwrite(stream_.Data(), 1, stream_.Size(), stderr);
			fflush(stderr);
			abort();
		}
	}

	static LogBuffer* CurrentLogBuffer()
	{
		return t_currentLogBuffer;
	}

	static void SetCurrentLogBuffer(LogBuffer* buf)
	{
		t_currentLogBuffer = buf;
	}

	LogStream& Stream()
	{
		return stream_;
	}

private:
	static thread_local LogBuffer* t_currentLogBuffer;

	LogStream stream_;
	const char* file_;
	int line_;
	LogLevel level_;
};

inline thread_local LogBuffer* Logger::t_currentLogBuffer = nullptr;

extern AsyncLogWriter* g_logWriter;           
extern std::vector<LogBuffer*> g_logBuffers;

#define LOG_TRACE Logger(__FILE__, __LINE__, LogLevel::TRACE).Stream()
#define LOG_DEBUG Logger(__FILE__, __LINE__, LogLevel::DEBUG).Stream()
#define LOG_INFO  Logger(__FILE__, __LINE__, LogLevel::INFO).Stream()
#define LOG_WARN  Logger(__FILE__, __LINE__, LogLevel::WARN).Stream()
#define LOG_ERROR Logger(__FILE__, __LINE__, LogLevel::ERROR).Stream()
#define LOG_FATAL Logger(__FILE__, __LINE__, LogLevel::FATAL).Stream()