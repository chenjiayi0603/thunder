/**
 * ClientMsgCodec 客户端消息编解码 E2E 测试
 */
#include "codec/ClientMsgCodec.hpp"
#include "codec/ClientMsgHead.hpp"
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>

// ========== ClientMsgCodec Encode/Decode 往返 ==========

TEST(ClientMsgCodec, Construction)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE);
    (void)codec;
    SUCCEED();
}

TEST(ClientMsgCodec, EncodeDecodeRoundTrip)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE);

    MsgHead head;
    head.set_cmd(100);
    head.set_seq(42);

    MsgBody body;
    body.set_sbody("hello_client");
    head.set_msgbody_len(body.ByteSize());

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);

    MsgHead decodedHead;
    MsgBody decodedBody;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decodedHead, decodedBody));
    EXPECT_EQ(100u, decodedHead.cmd());
    EXPECT_EQ(42u, decodedHead.seq());
    EXPECT_EQ("hello_client", decodedBody.sbody());
}

TEST(ClientMsgCodec, EncodeDecodeEmptyBody)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE);

    MsgHead head;
    head.set_cmd(1);
    head.set_seq(0);
    head.set_msgbody_len(0);
    MsgBody body;

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);

    MsgHead dh;
    MsgBody db;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, dh, db));
    EXPECT_EQ(1u, dh.cmd());
    EXPECT_EQ(0u, dh.msgbody_len());
}

TEST(ClientMsgCodec, DecodeTruncatedPauses)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE);

    MsgHead head;
    head.set_cmd(99);
    MsgBody body;
    body.set_sbody("data");
    head.set_msgbody_len(body.ByteSize());

    util::CBuffer buf;
    codec.Encode(head, body, &buf);

    // 截断——只取前 3 字节
    util::CBuffer partial;
    buf.Read(&partial, 3);

    MsgHead dh;
    MsgBody db;
    EXPECT_EQ(net::CODEC_STATUS_PAUSE, codec.Decode(&partial, dh, db));
}

TEST(ClientMsgCodec, DecodeCorruptedErrors)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE);
    util::CBuffer buf;
    char garbage[8] = {(char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF,
                       (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF};
    buf.Write(garbage, 8);

    MsgHead dh;
    MsgBody db;
    net::E_CODEC_STATUS st = codec.Decode(&buf, dh, db);
    EXPECT_NE(net::CODEC_STATUS_OK, st);
}

TEST(ClientMsgCodec, MultipleMessagesConsecutive)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE);
    util::CBuffer buf;

    for (int i = 1; i <= 3; ++i)
    {
        MsgHead h;
        h.set_cmd(1000 + i);
        h.set_seq(i);
        MsgBody b;
        b.set_sbody("msg_" + std::to_string(i));
        h.set_msgbody_len(b.ByteSize());
        codec.Encode(h, b, &buf);
    }

    for (int i = 1; i <= 3; ++i)
    {
        MsgHead dh;
        MsgBody db;
        EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, dh, db));
        EXPECT_EQ(1000u + i, dh.cmd());
        EXPECT_EQ(static_cast<uint32>(i), dh.seq());
        EXPECT_EQ("msg_" + std::to_string(i), db.sbody());
    }
    EXPECT_EQ(0u, buf.ReadableBytes());
}

TEST(ClientMsgCodec, LargeBodyRoundTrip)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE);

    MsgHead head;
    head.set_cmd(5000);
    head.set_seq(1);
    MsgBody body;
    std::string payload(200, 'X');
    body.set_sbody(payload);
    head.set_msgbody_len(body.ByteSize());

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));

    MsgHead dh;
    MsgBody db;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, dh, db));
    EXPECT_EQ(5000u, dh.cmd());
    EXPECT_EQ(payload, db.sbody());
}

TEST(ClientMsgCodec, KeyConstructor)
{
    net::ClientMsgCodec codec(util::CODEC_PRIVATE, "lt_key_42");
    MsgHead head;
    head.set_cmd(1);
    MsgBody body;
    body.set_sbody("keyed");
    head.set_msgbody_len(body.ByteSize());

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));
}
