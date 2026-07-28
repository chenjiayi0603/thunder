#include "codec/CodecMqtt.hpp"
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>
#include <cstring>

TEST(CodecMqtt, Construction) { net::CodecMqtt c(util::CODEC_MQTT); EXPECT_EQ(util::CODEC_MQTT, c.GetCodecType()); }
TEST(CodecMqtt, RemainingLength_0) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0x10,0x00}; b.Write(r,2); MsgHead h; MsgBody d;
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d)); EXPECT_EQ(net::MQTT_CMD(1),h.cmd()); }
TEST(CodecMqtt, RemainingLength_127) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[130]; r[0]=0x10; r[1]=0x7F; memset(r+2,0x42,127); b.Write(r,129); MsgHead h; MsgBody d;
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d)); EXPECT_EQ(0u,b.ReadableBytes()); }
TEST(CodecMqtt, RemainingLength_128) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[132]; r[0]=0x10;r[1]=0x80;r[2]=0x01;memset(r+3,0x42,128);b.Write(r,131);MsgHead h;MsgBody d;
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d));}
TEST(CodecMqtt, DecodeConnect) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0x10,0x0E,0x00,0x04,'M','Q','T','T',0x04,0x02,0x00,0x3C,0x00,0x04,'t','e'}; b.Write(r,sizeof(r));
    MsgHead h;MsgBody d; EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d)); EXPECT_EQ(net::MQTT_CMD(1),h.cmd());}
TEST(CodecMqtt, DecodePublishQos0) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0x30,0x08,0x00,0x03,'f','o','o','b','a','r'}; b.Write(r,sizeof(r));
    MsgHead h;MsgBody d; EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d));}
TEST(CodecMqtt, DecodePublishQos1) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0x32,0x0A,0x00,0x03,'f','o','o',0x00,0x01,'b','a','r'}; b.Write(r,sizeof(r));
    MsgHead h;MsgBody d; EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d)); EXPECT_EQ(0x32,(uint8_t)d.body()[0]);}
TEST(CodecMqtt, DecodeSubscribe) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0x82,0x08,0x00,0x01,0x00,0x03,'f','o','o',0x00}; b.Write(r,sizeof(r));
    MsgHead h;MsgBody d; EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d));}
TEST(CodecMqtt, DecodePingreq) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0xC0,0x00}; b.Write(r,2); MsgHead h;MsgBody d;
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d));}
TEST(CodecMqtt, DecodeDisconnect) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0xE0,0x00}; b.Write(r,2); MsgHead h;MsgBody d;
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h,d));}
TEST(CodecMqtt, EncodeConnack) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b; MsgHead h; MsgBody d;
    h.set_cmd(net::MQTT_CMD(2)); d.set_body(std::string(1,0)+std::string(1,0));
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Encode(h,d,&b)); EXPECT_EQ(4u,b.ReadableBytes());
    EXPECT_EQ(0x20,(uint8_t)b.GetRawReadBuffer()[0]);}
TEST(CodecMqtt, EncodePublishQos1) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b; MsgHead h; MsgBody d;
    h.set_cmd(net::MQTT_CMD(3)); h.set_seq(0x02); d.set_body("test");
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Encode(h,d,&b)); EXPECT_EQ(0x32,(uint8_t)b.GetRawReadBuffer()[0]);}
TEST(CodecMqtt, DecodePauseIncomplete) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b; uint8_t r=0x30; b.Write(&r,1);
    MsgHead h;MsgBody d; EXPECT_EQ(net::CODEC_STATUS_PAUSE,c.Decode(&b,h,d));}
TEST(CodecMqtt, DecodeMultiplePackets) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[]={0xC0,0x00,0xE0,0x00}; b.Write(r,4); MsgHead h1,h2;MsgBody d1,d2;
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h1,d1)); EXPECT_EQ(net::MQTT_CMD(12),h1.cmd());
    EXPECT_EQ(net::CODEC_STATUS_OK,c.Decode(&b,h2,d2)); EXPECT_EQ(net::MQTT_CMD(14),h2.cmd());}
TEST(CodecMqtt, MaxSize) {
    net::CodecMqtt c(util::CODEC_MQTT); util::CBuffer b;
    uint8_t r[3]={0x30,0x80,0x80}; r[0]=0x30;r[1]=0x80;r[2]=0x80; b.Write(r,3); MsgHead h;MsgBody d;
    EXPECT_EQ(net::CODEC_STATUS_PAUSE,c.Decode(&b,h,d));}
