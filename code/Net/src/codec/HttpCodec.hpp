/*******************************************************************************
 * Project:  Net
 * @file     HttpCodec.hpp
 * @brief 
 * @author   cjy
 * @date:    2019年10月6日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CODEC_HTTPCODEC_HPP_
#define SRC_CODEC_HTTPCODEC_HPP_

#include "util/http/http_parser.h"
#include "protocol/http.pb.h"
#include "ThunderCodec.hpp"
#include "google/protobuf/arena.h"

namespace net
{

/**
 * @brief HTTP 连接专属上下文
 * @note  挂在 tagConnectionAttr::pProtoCtx 上, 连接关闭时 delete
 *        Arena 预分配 8KB, 每个请求结束后 Reset() 复用
 */
struct HttpConnContext
{
    // Arena 默认构造, 首次使用自动分配 block (8KB+)
    google::protobuf::Arena arena;
    HttpConnContext() = default;
    ~HttpConnContext() = default;
};
//#define HTTP_CMD
/**
 * @brief http格式通信编码解码器
 */
class HttpCodec: public ThunderCodec
{
public:
	explicit HttpCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey = "That's a lizard.");
    virtual ~HttpCodec();

    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff,MsgHead& oMsgHead, MsgBody& oMsgBody)override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)override;

    virtual E_CODEC_STATUS Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff)override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg)override;

    /**
     * @brief 添加http头
     * @note 在encode前，允许框架根据连接属性添加http头
     */
    virtual void AddHttpHeader(const std::string& strHeaderName, const std::string& strHeaderValue);

    const std::string& ToString(const HttpMsg& oHttpMsg);
private:
    std::string m_strHttpString;
    std::unordered_map<std::string, std::string> m_mapAddingHttpHeader;       ///< encode前添加的http头，encode之后要清空
};

} /* namespace net */

#endif /* SRC_CODEC_HTTPCODEC_HPP_ */
