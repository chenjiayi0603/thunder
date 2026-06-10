#include "codec/WssCodec.hpp"
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>

TEST(WssCodec, Construction)
{
    net::WssCodec codec;
    (void)codec;
    SUCCEED();
}

TEST(WssCodec, SetSslConfig)
{
    net::WssCodec codec;
    net::WssCodec::SslConfig cfg;
    cfg.strServerCertFile = "/tmp/cert.pem";
    cfg.strServerKeyFile  = "/tmp/key.pem";
    codec.SetSslConfig(cfg);
    SUCCEED();
}

TEST(WssCodec, EncodeDecodeMsgHeadBody)
{
    net::WssCodec codec;
    MsgHead head;
    head.set_cmd(1);
    head.set_seq(100);
    MsgBody body;
    body.set_body("test");
    util::CBuffer buf;
    auto s = codec.Encode(head, body, &buf);
    EXPECT_EQ(s, net::CODEC_STATUS_OK);
    EXPECT_TRUE(buf.ReadableBytes() > 0);
}
