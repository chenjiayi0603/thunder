/**
 * ProtoCodec 编解码单元测试
 */
#include "codec/ProtoCodec.hpp"
#include <gtest/gtest.h>

TEST(ProtoCodec, Construction)
{
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL);
    (void)codec;
    SUCCEED();
}

TEST(ProtoCodec, EncodeDecodeRoundTrip)
{
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL);

    MsgHead head;
    head.set_cmd(1001);
    head.set_seq(42);

    MsgBody body;
    body.set_sbody("hello_protocodec");
    head.set_msgbody_len(body.ByteSize());

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);

    MsgHead decodedHead;
    MsgBody decodedBody;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decodedHead, decodedBody));
    EXPECT_EQ(1001u, decodedHead.cmd());
    EXPECT_EQ(42u, decodedHead.seq());
    EXPECT_EQ("hello_protocodec", decodedBody.sbody());
}

TEST(ProtoCodec, EncodeDecodeEmptyBody)
{
    // 心跳包：只有 msgbody_len=0，不解码 body
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL);
    MsgHead head;
    head.set_cmd(200);
    head.set_seq(0);
    head.set_msgbody_len(0);  // heartbeat: no body
    MsgBody body;

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));
    // 心跳包 head 15 字节，解码后 msgbody_len=0
    EXPECT_EQ(15u, buf.ReadableBytes());

    MsgHead decodedHead;
    MsgBody decodedBody;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decodedHead, decodedBody));
    EXPECT_EQ(200u, decodedHead.cmd());
    EXPECT_EQ(0u, decodedHead.msgbody_len());
}

TEST(ProtoCodec, DecodeTruncatedPauses)
{
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL);
    MsgHead head;
    head.set_cmd(999);
    MsgBody body;
    body.set_sbody("data");
    head.set_msgbody_len(body.ByteSize());

    util::CBuffer buf;
    codec.Encode(head, body, &buf);

    // 截断数据
    size_t originalReadable = buf.ReadableBytes();
    util::CBuffer partial;
    buf.Read(&partial, 5); // 只取 5 字节

    MsgHead dh;
    MsgBody db;
    EXPECT_EQ(net::CODEC_STATUS_PAUSE, codec.Decode(&partial, dh, db));
}

TEST(ProtoCodec, DecodeCorruptedErrors)
{
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL);
    util::CBuffer buf;
    char garbage[4] = {(char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF};
    buf.Write(garbage, 4);

    MsgHead dh;
    MsgBody db;
    net::E_CODEC_STATUS st = codec.Decode(&buf, dh, db);
    EXPECT_NE(net::CODEC_STATUS_OK, st);
}

TEST(ProtoCodec, MultipleMessagesConsecutive)
{
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL);
    util::CBuffer buf;

    for (int i = 1; i <= 3; ++i)
    {
        MsgHead h;
        h.set_cmd(1000 + i);
        h.set_seq(i);
        MsgBody b;
        b.set_sbody(std::to_string(i));
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
    }
    EXPECT_EQ(0u, buf.ReadableBytes());
}

TEST(ProtoCodec, LargeBodyRoundTrip)
{
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL);
    MsgHead head;
    head.set_cmd(5000);
    head.set_seq(1);
    MsgBody body;
    std::string payload(102, 'B');
    body.set_sbody(payload);
    head.set_msgbody_len(body.ByteSize());

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);

    MsgHead dh;
    MsgBody db;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, dh, db));
    EXPECT_EQ(5000u, dh.cmd());
    EXPECT_EQ(payload, db.sbody());
}

TEST(ProtoCodec, KeyConstructor)
{
    net::ProtoCodec codec(util::CODEC_PB_INTERNAL, "custom_key_for_test");
    MsgHead head;
    head.set_cmd(1);
    MsgBody body;
    body.set_sbody("keytest");
    head.set_msgbody_len(body.ByteSize());
    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(head, body, &buf));
}
