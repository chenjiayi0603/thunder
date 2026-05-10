/**
 * CBuffer 环形缓冲区单元测试
 */
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>
#include <cstring>

TEST(CBuffer, DefaultConstruction)
{
    util::CBuffer buf;
    EXPECT_EQ(0u, buf.ReadableBytes());
    EXPECT_EQ(0u, buf.WriteableBytes());
    EXPECT_FALSE(buf.Readable());
}

TEST(CBuffer, WriteAndReadRoundTrip)
{
    util::CBuffer buf;
    const char* data = "hello";
    int written = buf.Write(data, 5);
    EXPECT_EQ(5, written);
    EXPECT_EQ(5u, buf.ReadableBytes());
    EXPECT_TRUE(buf.Readable());

    char out[10] = {};
    int rd = buf.Read(out, 5);
    EXPECT_EQ(5, rd);
    EXPECT_STREQ("hello", out);
    EXPECT_EQ(0u, buf.ReadableBytes());
}

TEST(CBuffer, WriteByteAndReadByte)
{
    util::CBuffer buf;
    EXPECT_TRUE(buf.WriteByte('A'));
    char ch = 0;
    EXPECT_TRUE(buf.ReadByte(ch));
    EXPECT_EQ('A', ch);
}

TEST(CBuffer, SizeConstruction)
{
    util::CBuffer buf(1024);
    EXPECT_GE(buf.Capacity(), 1024u);
}

TEST(CBuffer, Clear)
{
    util::CBuffer buf;
    buf.Write("test", 4);
    buf.Clear();
    EXPECT_EQ(0u, buf.ReadableBytes());
}

TEST(CBuffer, EnsureWritableGrows)
{
    util::CBuffer buf;
    EXPECT_TRUE(buf.EnsureWritableBytes(100));
    EXPECT_GE(buf.Capacity(), 100u);
}

TEST(CBuffer, ReserveSameAsEnsureWritable)
{
    util::CBuffer buf;
    EXPECT_TRUE(buf.Reserve(256));
    EXPECT_GE(buf.Capacity(), 256u);
}

TEST(CBuffer, ReadMoreThanAvailable)
{
    util::CBuffer buf;
    buf.Write("ab", 2);
    char out[10] = {};
    EXPECT_EQ(-1, buf.Read(out, 10));
    EXPECT_EQ(2u, buf.ReadableBytes());
}

TEST(CBuffer, EmptyBufferReadByte)
{
    util::CBuffer buf;
    char ch;
    EXPECT_FALSE(buf.ReadByte(ch));
}

TEST(CBuffer, WriteEmptyData)
{
    util::CBuffer buf;
    int written = buf.Write("", 0);
    EXPECT_EQ(0, written);
}

TEST(CBuffer, SetBytesValid)
{
    util::CBuffer buf;
    buf.Write("12345", 5);
    EXPECT_EQ(2, buf.SetBytes((void*)"XY", 2, 1));
    EXPECT_EQ('X', buf.GetRawReadBuffer()[1]);
    EXPECT_EQ('Y', buf.GetRawReadBuffer()[2]);
}

TEST(CBuffer, SetBytesOutOfRange)
{
    util::CBuffer buf;
    buf.Write("hi", 2);
    EXPECT_EQ(-1, buf.SetBytes((void*)"X", 1, 5));
}

TEST(CBuffer, CopyoutPreservesData)
{
    util::CBuffer buf;
    buf.Write("copy_test", 9);
    char out[20] = {};
    EXPECT_EQ(9, buf.Copyout(out, 20));
    EXPECT_STREQ("copy_test", out);
    EXPECT_EQ(9u, buf.ReadableBytes()); // Copyout 不消耗
}

TEST(CBuffer, CrossBufferWrite)
{
    util::CBuffer src;
    src.Write("source", 6);
    util::CBuffer dst;
    EXPECT_EQ(6, dst.Write(&src, 6));
    EXPECT_EQ(0u, src.ReadableBytes());
    EXPECT_EQ(6u, dst.ReadableBytes());
}

TEST(CBuffer, CrossBufferRead)
{
    util::CBuffer src;
    src.Write("hello", 5);
    util::CBuffer dst;
    EXPECT_EQ(5, src.Read(&dst, 5));
    char out[10] = {};
    dst.Read(out, 5);
    EXPECT_STREQ("hello", out);
}

TEST(CBuffer, ToString)
{
    util::CBuffer buf;
    buf.Write("world", 5);
    EXPECT_EQ("world", buf.ToString());
}

TEST(CBuffer, LargeDataRoundTrip)
{
    util::CBuffer buf;
    std::string large(10000, '\0');
    for (int i = 0; i < 10000; ++i)
        large[i] = static_cast<char>('A' + (i % 26));

    int wr = buf.Write(large.data(), large.size());
    EXPECT_EQ(10000, wr);
    std::string out(10000, '\0');
    int rd = buf.Read(&out[0], 10000);
    EXPECT_EQ(10000, rd);
    EXPECT_EQ(large, out);
}

TEST(CBuffer, SkipBytes)
{
    util::CBuffer buf;
    buf.Write("0123456789", 10);
    buf.SkipBytes(5);
    EXPECT_EQ(5u, buf.ReadableBytes());
    EXPECT_EQ('5', buf.GetRawReadBuffer()[0]);
}

TEST(CBuffer, Limit)
{
    util::CBuffer buf;
    buf.Write("hello", 5);
    buf.Limit();
    EXPECT_EQ(5u, buf.Capacity());
    EXPECT_EQ(5u, buf.ReadableBytes());
}


TEST(CBuffer, DiscardReadedBytes)
{
    util::CBuffer buf;
    buf.Write("0123456789", 10);
    char tmp[5];
    buf.Read(tmp, 5);
    buf.DiscardReadedBytes();
    EXPECT_EQ(5u, buf.ReadableBytes());
    EXPECT_EQ('5', buf.GetRawReadBuffer()[0]);
}

TEST(CBuffer, GetRawReadWriteBuffer)
{
    util::CBuffer buf;
    buf.Write("abcd", 4);
    EXPECT_EQ('a', buf.GetRawReadBuffer()[0]);
    buf.GetRawWriteBuffer();
}
