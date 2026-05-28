/*******************************************************************************
 * Project:  Net
 * @file     ProtoCodec.hpp
 * @brief 
 * @author   cjy
 * @date:    2019年10月6日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CODEC_PROTOCODEC_HPP_
#define SRC_CODEC_PROTOCODEC_HPP_

#include "ThunderCodec.hpp"
#include "google/protobuf/arena.h"

namespace net
{

/**
 * @brief Internal PB 连接专属上下文 (每连接一个, 挂在 tagConnectionAttr::pProtoCtx 上)
 * @note  Arena 用于消纳 ParseFromArray 期间的所有子对象分配 (repeated field / string / nested msg),
 *        将 N 次散列 malloc 替换为 Arena 内部 bump pointer 前进。
 *        请求结束后 CopyFrom 到栈对象, 再 Reset() 复用 Arena 内存块。
 */
struct ProtoConnContext
{
    google::protobuf::Arena arena;
    ProtoConnContext() = default;
    ~ProtoConnContext() = default;
};
/**
 * @brief pb格式通信编码解码器
 */
class ProtoCodec: public ThunderCodec
{
public:
    ProtoCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey = "That's a lizard.");
    virtual ~ProtoCodec();
    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)override;
};

} /* namespace net */

#endif /* SRC_CODEC_PROTOCODEC_HPP_ */
