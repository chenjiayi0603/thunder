/*******************************************************************************
 * Project:  Net
 * @file     AppMsgCodec.hpp
 * @brief    与APP通信协议编解码器
 * @author   cjy
 * @date:    2019年10月9日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CODEC_APPMSGCODEC_HPP_
#define SRC_CODEC_APPMSGCODEC_HPP_

#include "ThunderCodec.hpp"

namespace net
{

/**
 * @brief 与客户端通信编码解码器
 */
class AppMsgCodec: public ThunderCodec
{
public:
	AppMsgCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey = "client key");
    virtual ~AppMsgCodec();

    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)override;

    void TestEncodeDecodeAes();
};

} /* namespace net */

#endif /* SRC_CODEC_CLIENTMSGCODEC_HPP_ */
