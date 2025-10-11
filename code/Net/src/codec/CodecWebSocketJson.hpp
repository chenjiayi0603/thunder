/*******************************************************************************
 * Project:  Net
 * @file     CodecWebSocketJson.hpp
 * @brief    与手机客户端通信协议编解码器
 * @author   Tommy
 * @date:    2016年9月3日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CODEC_CODECWSEXTENTJSON_HPP_
#define SRC_CODEC_CODECWSEXTENTJSON_HPP_
#include <string>
#include <map>
#include "util/http/http_parser.h"
#include "util/encrypt/base64.h"
#include "protocol/http.pb.h"
#include "ThunderCodec.hpp"
#include "ClientMsgHead.hpp"
#include "CodecCommon.hpp"

namespace net
{

/**
 * @brief CodecWebSocketJson websocket编码解码器，json消息体
 * @note 可以解析 http请求以及websocket请求
 */
class CodecWebSocketJson: public ThunderCodec
{
public:
	explicit CodecWebSocketJson(util::E_CODEC_TYPE eCodecType, const std::string& strKey = "That's a lizard.");
    virtual ~CodecWebSocketJson();
    /**
     * @brief 解码编码websocket请求
     * @note Decode解码时判别是http请求还是websocket请求来处理,因为是框架固定调用该函数
     */
    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead,const MsgBody& oMsgBody, util::CBuffer* pBuff)override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)override;
    /**
	 * @brief 解码编码http请求
	 */
    virtual E_CODEC_STATUS Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff)override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg)override;
private:
    E_CODEC_STATUS EncodeHandShake(const HttpMsg& oHttpMsg,util::CBuffer* pBuff);
    E_CODEC_STATUS EncodeHttp(const HttpMsg& oHttpMsg, util::CBuffer* pBuff);
private:
    /**
	 * @brief 解析http请求
	 */
    static int OnMessageBegin(http_parser *parser);
    static int OnUrl(http_parser *parser, const char *at, size_t len);
    static int OnStatus(http_parser *parser, const char *at, size_t len);
    static int OnHeaderField(http_parser *parser, const char *at, size_t len);
    static int OnHeaderValue(http_parser *parser, const char *at, size_t len);
    static int OnHeadersComplete(http_parser *parser);
    static int OnBody(http_parser *parser, const char *at, size_t len);
    static int OnMessageComplete(http_parser *parser);
    static int OnChunkHeader(http_parser *parser);
    static int OnChunkComplete(http_parser *parser);
	/*
     * @brief oHttpMsg序列化为字符串
     * */
    const std::string& ToString(const HttpMsg& oHttpMsg);
    /*
	 * @brief Base64编码解码
	 * */
    void Base64Encode(const char* data,unsigned int datalen,std::string &strEncode);
    void Base64Decode(const char* encoded,unsigned int encodedlen,std::string &strDecode);

    http_parser_settings m_parser_setting;
    http_parser m_parser;
    std::string m_strHttpString;
    std::unordered_map<std::string, std::string> m_mapAddingHttpHeader;       ///< encode前添加的http头，encode之后要清空
};

} /* namespace net */

#endif /* SRC_CODEC_CODECWSEXTENTJSON_HPP_ */
