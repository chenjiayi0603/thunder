/*******************************************************************************
 * Project:  Net
 * @file     ClientMsgCodec.hpp
 * @brief    与手机客户端通信协议编解码器
 * @author   Tommy
 * @date:    2019年10月9日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CODEC_CLIENTMSGCODEC_HPP_
#define SRC_CODEC_CLIENTMSGCODEC_HPP_

#include "ThunderCodec.hpp"

namespace net
{

/**
 * @brief 与客户端通信编码解码器
 */
class ClientMsgCodec: public ThunderCodec
{
public:
    ClientMsgCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey = "client key");
    virtual ~ClientMsgCodec();

    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)override;
};

} /* namespace net */

#endif /* SRC_CODEC_CLIENTMSGCODEC_HPP_ */
