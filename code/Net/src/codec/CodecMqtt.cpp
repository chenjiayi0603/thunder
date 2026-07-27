/**
 * CodecMqtt.cpp - MQTT 3.1.1 编解码器 (基于 Eclipse Paho MQTTPacket)
 */
#include "CodecMqtt.hpp"
extern "C" {
#include "MQTTPacket.h"
}
#include "log4cplus/loggingmacros.h"
#include "labor/Attribution.hpp"
#include "NetDefine.hpp"
#include <cstring>

namespace net
{

CodecMqtt::CodecMqtt(util::E_CODEC_TYPE eCodecType, const std::string& strKey)
    : ThunderCodec(eCodecType, strKey) {}
CodecMqtt::~CodecMqtt() {}

E_CODEC_STATUS CodecMqtt::Decode(tagConnectionAttr* pConn, MsgHead& oMsgHead, MsgBody& oMsgBody)
{ return Decode(pConn->pRecvBuff.get(), oMsgHead, oMsgBody); }

E_CODEC_STATUS CodecMqtt::Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    size_t readable = pBuff->ReadableBytes();
    if (readable < 2) return CODEC_STATUS_PAUSE;

    const char* raw = pBuff->GetRawReadBuffer();
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(raw);

    uint8_t typeFlags = bytes[0];
    uint8_t packetType = (typeFlags >> 4) & 0x0F;

    unsigned int remainingLen = 0;
    int rlBytes = MQTTPacket_decodeBuf(const_cast<char*>(raw + 1), &remainingLen);
    if (rlBytes < 0) return CODEC_STATUS_ERR;

    uint32_t headerLen = 1 + rlBytes;
    uint32_t totalLen = headerLen + remainingLen;
    if (readable < totalLen) return CODEC_STATUS_PAUSE;

    // 最大包大小限制
    if (remainingLen > 1048576) {
        LOG4_ERROR("MQTT packet too large: %u", remainingLen);
        return CODEC_STATUS_ERR;
    }

    oMsgHead.set_cmd(MQTT_CMD(packetType));
    oMsgHead.set_seq(0);
    oMsgBody.set_body(raw, totalLen);
    pBuff->SkipBytes(totalLen);

    LOG4_TRACE("MQTT decode: type=%u remainingLen=%u", packetType, remainingLen);
    return CODEC_STATUS_OK;
}

E_CODEC_STATUS CodecMqtt::Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    uint32_t cmd = oMsgHead.cmd();
    // 逆运算: cmd = MQTT_CMD_BASE + t*2 - 1 → t = (cmd - MQTT_CMD_BASE + 1) / 2
    uint8_t packetType = static_cast<uint8_t>((cmd - MQTT_CMD_BASE + 1) / 2);

    const std::string& body = oMsgBody.body();
    uint32_t bodyLen = static_cast<uint32_t>(body.size());

    // 编码固定头: type+flags(1) + remaining_length(variable)
    uint8_t typeFlags = static_cast<uint8_t>(packetType << 4);
    if (packetType == MQTT_PUBLISH)
        typeFlags |= (oMsgHead.seq() & 0x0F); // QoS/DUP/Retain flags

    char header[5];
    header[0] = static_cast<char>(typeFlags);
    int hdrLen = 1 + MQTTPacket_encode(header + 1, bodyLen);

    pBuff->Write(header, hdrLen);
    if (bodyLen > 0)
        pBuff->Write(body.data(), bodyLen);

    return CODEC_STATUS_OK;
}

} /* namespace net */
