#pragma once

#include <string>
#include <cstdio>
#include <cstring>

class LogStream
{
public:
    LogStream() : cur_(buffer_) {}

    // operator<< 重载
    LogStream& operator<<(bool v)
    {
        if (v)
        {
            Append("true", 4);
        }
        else
        {
            Append("false", 5);
        }
        return *this;
    }

    LogStream& operator<<(short v)
    {
        FormatInteger(static_cast<long long>(v));
        return *this;
    }

    LogStream& operator<<(unsigned short v)
    {
        FormatInteger(static_cast<unsigned long long>(v));
        return *this;
    }

    LogStream& operator<<(int v)
    {
        FormatInteger(static_cast<long long>(v));
        return *this;
    }

    LogStream& operator<<(unsigned int v)
    {
        FormatInteger(static_cast<unsigned long long>(v));
        return *this;
    }

    LogStream& operator<<(long v)
    {
        FormatInteger(static_cast<long long>(v));
        return *this;
    }

    LogStream& operator<<(unsigned long v)
    {
        FormatInteger(static_cast<unsigned long long>(v));
        return *this;
    }

    LogStream& operator<<(long long v)
    {
        FormatInteger(v);
        return *this;
    }

    LogStream& operator<<(unsigned long long v)
    {
        FormatInteger(v);
        return *this;
    }

    LogStream& operator<<(float v)
    {
        if (Avail() >= 32)
        {
            int len = snprintf(cur_, Avail(), "%.12f", v);
            if (len > 0) cur_ += len;
        }
        return *this;
    }

    LogStream& operator<<(double v)
    {
        if (Avail() >= 32)
        {
            int len = snprintf(cur_, Avail(), "%.12g", v);
            if (len > 0) cur_ += len;
        }
        return *this;
    }

    LogStream& operator<<(char v)
    {
        if (Avail() >= 1)
        {
            *cur_++ = v;
        }
        return *this;
    }

    LogStream& operator<<(const char* str)
    {
        if (str)
        {
            Append(str, strlen(str));
        }
        else
        {
            Append("(null)", 6);
        }
        return *this;
    }

    LogStream& operator<<(const std::string& str)
    {
        Append(str.c_str(), str.length());
        return *this;
    }

    LogStream& operator<<(const void* ptr)
    {
        if (Avail() >= 20)
        {
            int len = snprintf(cur_, Avail(), "0x%lx",
                reinterpret_cast<unsigned long>(ptr));
            if (len > 0) cur_ += len;
        }
        return *this;
    }

    void Reset()
    {
        cur_ = buffer_;
    }

    const char* Data() const
    {
        return buffer_;
    }

    size_t Size() const
    {
        return cur_ - buffer_;
    }

private:
    char  buffer_[4096];
    char* cur_;

    size_t Avail() const
    {
        return buffer_ + 4096 - cur_;
    }

    void Append(const char* data, size_t len)
    {
        size_t avail = Avail();
        if (avail > 0)
        {
            size_t copyLen = (len < avail) ? len : avail;
            memcpy(cur_, data, copyLen);
            cur_ += copyLen;
        }
    }

    void FormatInteger(long long v)
    {
        if (Avail() < 32) return;

        
        if (v < 0)
        {
            *cur_++ = '-';
            
            unsigned long long absVal =
                static_cast<unsigned long long>(-(v + 1)) + 1;
            FormatInteger(absVal);
            return;
        }

        
        FormatInteger(static_cast<unsigned long long>(v));
    }

    void FormatInteger(unsigned long long v)
    {
        if (Avail() < 32) return;

        char temp[32];
        char* p = temp + sizeof(temp);  

        if (v == 0)
        {
            *--p = '0';
        }
        else
        {
            while (v > 0)
            {
                *--p = '0' + (v % 10);  
                v /= 10;                
            }
        }

        size_t len = temp + sizeof(temp) - p;
        memcpy(cur_, p, len);
        cur_ += len;
    }
};