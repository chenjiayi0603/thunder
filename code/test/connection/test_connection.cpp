/**
 * ConnectionAttr 连接属性单元测试
 */
#include "labor/types/ConnectionAttr.hpp"
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <cstring>

TEST(ConnectionAttr, DefaultConstruction)
{
    tagConnectionAttr attr;
    EXPECT_EQ(0, attr.iFd);
    EXPECT_EQ(0u, attr.ulSeq);
    EXPECT_EQ(0u, attr.ulForeignSeq);
    EXPECT_EQ(nullptr, attr.pRecvBuff);
    EXPECT_EQ(nullptr, attr.pSendBuff);
    EXPECT_EQ(nullptr, attr.pIoWatcher);
    EXPECT_EQ(nullptr, attr.pTimeWatcher);
    EXPECT_TRUE(attr.strIdentify.empty());
    EXPECT_TRUE(attr.strSessionKey.empty());
}

TEST(ConnectionAttr, SetFdAndSeq)
{
    tagConnectionAttr attr;
    attr.iFd = 42;
    attr.ulSeq = 100;
    EXPECT_EQ(42, attr.iFd);
    EXPECT_EQ(100u, attr.ulSeq);
}

TEST(ConnectionAttr, RecvBuffLifecycle)
{
    tagConnectionAttr attr;
    attr.pRecvBuff = std::make_unique<util::CBuffer>();
    ASSERT_NE(nullptr, attr.pRecvBuff);
    attr.pRecvBuff->Write("data", 4);
    EXPECT_EQ(4u, attr.pRecvBuff->ReadableBytes());
}

TEST(ConnectionAttr, SendBuffLifecycle)
{
    tagConnectionAttr attr;
    attr.pSendBuff = std::make_unique<util::CBuffer>();
    ASSERT_NE(nullptr, attr.pSendBuff);
    attr.pSendBuff->Write("out", 3);
    EXPECT_EQ(3u, attr.pSendBuff->ReadableBytes());
}

TEST(ConnectionAttr, Identify)
{
    tagConnectionAttr attr;
    attr.strIdentify = "logic:192.168.1.1:8080.0";
    EXPECT_EQ("logic:192.168.1.1:8080.0", attr.strIdentify);
}

TEST(ConnectionAttr, RemoteAddr)
{
    tagConnectionAttr attr;
    std::strncpy(attr.szRemoteAddr, "10.0.0.1", sizeof(attr.szRemoteAddr) - 1);
    EXPECT_STREQ("10.0.0.1", attr.szRemoteAddr);
}

TEST(ConnectionAttr, SessionKey)
{
    tagConnectionAttr attr;
    attr.strSessionKey = "sk_test_123";
    EXPECT_EQ("sk_test_123", attr.strSessionKey);
}

TEST(ConnectionAttr, SeqMismatchDetection)
{
    // ABA 防护：验证相同 fd 不同 seq 的分辨逻辑
    tagConnectionAttr attr1;
    attr1.iFd = 10;
    attr1.ulSeq = 100;

    tagConnectionAttr attr2;
    attr2.iFd = 10;   // 相同 fd
    attr2.ulSeq = 200; // 不同 seq

    EXPECT_EQ(attr1.iFd, attr2.iFd);
    EXPECT_NE(attr1.ulSeq, attr2.ulSeq);
}
