#include <gtest/gtest.h>
#include "buffer.h"
#include <cstring>

// ============================================================
// 用例 1：初始状态
// ============================================================
TEST(BufferTest, InitialState)
{
    Buffer buf;

    EXPECT_EQ(buf.ReadableBytes(), 0);
    EXPECT_EQ(buf.WritableBytes(), 1024);
    EXPECT_EQ(buf.PrependableBytes(), 8);
    EXPECT_NE(buf.Peek(), nullptr);
}

// ============================================================
// 用例 2：基本写入 + 消费
// ============================================================
TEST(BufferTest, AppendAndRead)
{
    Buffer buf;

    buf.Append("hello", 5);

    EXPECT_EQ(buf.ReadableBytes(), 5);
    EXPECT_EQ(buf.WritableBytes(), 1024 - 5);
    EXPECT_EQ(std::strncmp(buf.Peek(), "hello", 5), 0);

    buf.Retrieve(3);

    EXPECT_EQ(buf.ReadableBytes(), 2);
    EXPECT_EQ(std::strncmp(buf.Peek(), "lo", 2), 0);
}

// ============================================================
// 用例 3：写入 std::string
// ============================================================
TEST(BufferTest, AppendString)
{
    Buffer buf;

    buf.Append(std::string("world"));

    EXPECT_EQ(buf.ReadableBytes(), 5);
    EXPECT_EQ(std::strncmp(buf.Peek(), "world", 5), 0);
}

// ============================================================
// 用例 4：全部消费
// ============================================================
TEST(BufferTest, RetrieveAll)
{
    Buffer buf;

    buf.Append("hello", 5);
    buf.RetrieveAll();

    EXPECT_EQ(buf.ReadableBytes(), 0);
    EXPECT_EQ(buf.WritableBytes(), 1024);
    EXPECT_EQ(buf.PrependableBytes(), 8);
}

// ============================================================
// 用例 5：消费并返回字符串
// ============================================================
TEST(BufferTest, RetrieveAsString)
{
    Buffer buf;

    buf.Append("hello", 5);

    std::string s = buf.RetrieveAsString(3);

    EXPECT_EQ(s, "hel");
    EXPECT_EQ(buf.ReadableBytes(), 2);

    std::string s2 = buf.RetrieveAllAsString();

    EXPECT_EQ(s2, "lo");
    EXPECT_EQ(buf.ReadableBytes(), 0);
}

// ============================================================
// 用例 6：头部插入
// ============================================================
TEST(BufferTest, Prepend)
{
    Buffer buf;

    buf.Append("world", 5);   // readIndex=8, writeIndex=13
    buf.Prepend("hello", 5);  // readIndex=8-5=3

    EXPECT_EQ(buf.ReadableBytes(), 10);
    EXPECT_EQ(std::strncmp(buf.Peek(), "helloworld", 10), 0);
}

// ============================================================
// 用例 7：头部插入越界
// ============================================================
TEST(BufferTest, PrependOverflow)
{
    Buffer buf;

    EXPECT_THROW(
        {
            buf.Prepend("123456789", 9);  // PrependableBytes == 8，越界
        },
        std::out_of_range
    );
}

// ============================================================
// 用例 8：自动扩容
// ============================================================
TEST(BufferTest, AutoExpand)
{
    Buffer buf;

    std::string big(2000, 'x');

    buf.Append(big.data(), big.size());

    EXPECT_EQ(buf.ReadableBytes(), 2000);
    EXPECT_GT(buf.InternalCapacity(), 1024 + 8);
    EXPECT_EQ(std::strncmp(buf.Peek(), big.data(), 2000), 0);
}

// ============================================================
// 用例 9：内部碎片整理
// ============================================================
TEST(BufferTest, InternalMove)
{
    Buffer buf;

    buf.Append(std::string(1000, 'a').data(), 1000);
    buf.Retrieve(900);
    // 此时 readIndex=908 可读 100 字节
    // 可写空间只剩 1024+8-908-100=224
    // 但 readIndex 前面有 900 字节碎片

    EXPECT_EQ(buf.ReadableBytes(), 100);

    buf.Append(std::string(500, 'b').data(), 500);
    // 碎片整理后应当把 100 字节搬到前面，然后写入 500

    EXPECT_EQ(buf.ReadableBytes(), 600);
    EXPECT_EQ(std::strncmp(buf.Peek(),       std::string(100, 'a').data(), 100), 0);
    EXPECT_EQ(std::strncmp(buf.Peek() + 100, std::string(500, 'b').data(), 500), 0);
}

// ============================================================
// 用例 10：查找 CRLF
// ============================================================
TEST(BufferTest, FindCRLF)
{
    Buffer buf;

    buf.Append("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 40);

    const char* crlf = buf.FindCRLF();
    EXPECT_NE(crlf, nullptr);
    EXPECT_EQ(std::strncmp(buf.Peek(), "GET / HTTP/1.1", crlf - buf.Peek()), 0);

    buf.RetrieveUntil(crlf + 2);  // 跳过第一行

    const char* crlf2 = buf.FindCRLF();
    EXPECT_NE(crlf2, nullptr);
    EXPECT_EQ(std::strncmp(buf.Peek(), "Host: localhost", crlf2 - buf.Peek()), 0);
}

// ============================================================
// 用例 11：没找到 CRLF
// ============================================================
TEST(BufferTest, FindCRLFNotFound)
{
    Buffer buf;

    buf.Append("no newline here", 15);

    EXPECT_EQ(buf.FindCRLF(), nullptr);
}

// ============================================================
// 用例 12：交换两个 Buffer
// ============================================================
TEST(BufferTest, Swap)
{
    Buffer buf1;
    Buffer buf2;

    buf1.Append("aaaa", 4);
    buf2.Append("bbbb", 4);

    buf1.Swap(buf2);

    EXPECT_EQ(buf1.ReadableBytes(), 4);
    EXPECT_EQ(std::strncmp(buf1.Peek(), "bbbb", 4), 0);

    EXPECT_EQ(buf2.ReadableBytes(), 4);
    EXPECT_EQ(std::strncmp(buf2.Peek(), "aaaa", 4), 0);
}

// ============================================================
// 用例 13：收缩容量
// ============================================================
TEST(BufferTest, Shrink)
{
    Buffer buf;

    buf.Append(std::string(100, 'x').data(), 100);
    buf.Shrink();

    // Shrink 后容量应该接近实际用量（不严格等于，取决于 vector 实现）
    EXPECT_LE(buf.InternalCapacity(), 1024 + 8);
}
