/*******************************************************************************
 * Project:  Net
 * @file     CodecMqtt.hpp
 * @brief    MQTT 3.1.1 协议编解码器 (P0+P1)
 ******************************************************************************/
#ifndef SRC_CODEC_CODECMQTT_HPP_
#define SRC_CODEC_CODECMQTT_HPP_

#include "ThunderCodec.hpp"
#include <cstdint>
#include <string>

namespace net
{

enum EMqttPacketType : uint8_t
{
    MQTT_CONNECT     = 1,  MQTT_CONNACK  = 2,  MQTT_PUBLISH   = 3,
    MQTT_PUBACK      = 4,  MQTT_PUBREC   = 5,  MQTT_PUBREL    = 6,
    MQTT_PUBCOMP     = 7,  MQTT_SUBSCRIBE= 8,  MQTT_SUBACK    = 9,
    MQTT_UNSUBSCRIBE = 10, MQTT_UNSUBACK = 11, MQTT_PINGREQ   = 12,
    MQTT_PINGRESP    = 13, MQTT_DISCONNECT= 14,
};

enum EMqttConnackCode : uint8_t
{
    MQTT_CONNACK_ACCEPTED              = 0,
    MQTT_CONNACK_REFUSED_PROTO_VER     = 1,
    MQTT_CONNACK_REFUSED_ID_REJECTED   = 2,
    MQTT_CONNACK_REFUSED_SERVER_UNAVAIL= 3,
    MQTT_CONNACK_REFUSED_BAD_USER_PASS = 4,
    MQTT_CONNACK_REFUSED_NOT_AUTHORIZED= 5,
};

// ═══════════════════════════════════════════════════════════════════════════
// MQTT cmd 编码 & 框架分发机制
// ═══════════════════════════════════════════════════════════════════════════
//
// MQTT cmd 编码公式: MQTT_CMD(t) = MQTT_CMD_BASE + t*2 - 1
//   (t = MQTT 包类型: CONNECT=1, PUBLISH=3, SUBSCRIBE=8, ...)
//   例: PUBLISH(t=3) → 21000 + 3*2 - 1 = 21005 (奇数 = 请求)
//
// 为什么全部映射为奇数?
//   Thunder 框架用 gc_uiCmdReq(=0x1) & cmd 判断请求/响应:
//     奇数 → 请求 → 走 mapSo 查找 → AnyMessage 处理
//     偶数 → 响应 → 走 Step 回调
//   MQTT 所有包对 Broker 来说都是"请求"(需要处理), 所以全部用奇数。
//
// 为什么注册 cmd=501 (CMD_REQ_FROM_CLIENT)?
//   ┌──────────────────────────────────────────────────────┐
//   │ Worker::Dispose 消息分发:                             │
//   │                                                      │
//   │   mapSo.find(cmd=21005)  ← PUBLISH, 没人注册这个 cmd   │
//   │        ↓ 找不到                                       │
//   │   mapSo.find(cmd=21001)  ← CONNECT, 也没人注册          │
//   │        ↓ 全部 21001~21027 都找不到                     │
//   │   mapSo.find(501)        ← CMD_REQ_FROM_CLIENT       │
//   │        ↓ 找到了!                                      │
//   │   ModuleMqttBroker::AnyMessage() ← 处理所有 MQTT 消息  │
//   │                                                      │
//   │   CMD_REQ_FROM_CLIENT(501): 框架兜底常量,             │
//   │   "客户端发来的无匹配 cmd 的消息, 原样转发不改变 cmd"    │
//   └──────────────────────────────────────────────────────┘
//
//   简单说: MQTT 的 cmd 值(21001~21027)故意不与任何具体 Cmd 匹配,
//   全部漏到 501 这个万能兜底, 由 ModuleMqttBroker 统一接管。

// MQTT cmd 编码: 所有 MQTT 包类型映射为奇数 cmd (Thunder 框架奇数=请求, 偶数=响应)
// 公式: 21000 + t*2 - 1 → 全部为奇数, 确保走 Dispose 的"请求"路径
constexpr uint32_t MQTT_CMD_BASE = 21000;
inline constexpr uint32_t MQTT_CMD(uint8_t t) { return MQTT_CMD_BASE + static_cast<uint32_t>(t) * 2 - 1; }

class CodecMqtt : public ThunderCodec
{
public:
    explicit CodecMqtt(util::E_CODEC_TYPE eCodecType, const std::string& strKey = "");
    virtual ~CodecMqtt();
    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff) override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody) override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn, MsgHead& oMsgHead, MsgBody& oMsgBody) override;
private:
    void EncodeFixedHeader(uint8_t packetType, uint8_t flags, uint32_t remainingLen, util::CBuffer* pBuff);
    bool DecodeRemainingLength(const uint8_t*& pBuf, const uint8_t* pEnd,
                               uint32_t& remainingLen, uint8_t& remainingLenBytes);
};

} /* namespace net */
#endif
