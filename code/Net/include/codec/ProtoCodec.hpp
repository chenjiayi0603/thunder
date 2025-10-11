/*******************************************************************************
 * Project:  Net
 * @file     ProtoCodec.hpp
 * @brief 
 * @author   Tommy
 * @date:    2019年10月6日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CODEC_PROTOCODEC_HPP_
#define SRC_CODEC_PROTOCODEC_HPP_

#include "ThunderCodec.hpp"

namespace net
{
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
