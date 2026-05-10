/*******************************************************************************
 * Project:  Net
 * @file     CodecWebSocketJson.hpp
 * @brief    与手机客户端通信协议编解码器
 * @author   cjy
 * @date:    2016年9月3日
 * @note
 * Modify history:
 ******************************************************************************/
#include <netinet/in.h>
#include "CodecWebSocketPb.hpp"

namespace net
{

//http parse
int CodecWebSocketPb::OnMessageBegin(http_parser *parser)
{
    HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
    pHttpMsg->set_is_decoding(true);
    return (0);
}

int CodecWebSocketPb::OnUrl(http_parser *parser, const char *at, size_t len)
{
    HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
    pHttpMsg->set_url(at, len);
    struct http_parser_url stUrl;
    if (0 == http_parser_parse_url(at, len, 0, &stUrl))
    {
        if (stUrl.field_set & (1 << UF_PATH))
        {
            char *path = static_cast<char*>(malloc(stUrl.field_data[UF_PATH].len));
            strncpy(path, at + stUrl.field_data[UF_PATH].off,
                            stUrl.field_data[UF_PATH].len);
            pHttpMsg->set_path(path, stUrl.field_data[UF_PATH].len);
            free(path);
        }
    }
    return 0;
}

int CodecWebSocketPb::OnStatus(http_parser *parser, const char *at, size_t len)
{
    HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
    pHttpMsg->set_status_code(parser->status_code);
    return (0);
}

int CodecWebSocketPb::OnHeaderField(http_parser *parser, const char *at,
                size_t len)
{
	HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
	pHttpMsg->set_body(at, len);        // 用body暂存head_name，解析完head_value后再填充到head里
    return (0);
}

int CodecWebSocketPb::OnHeaderValue(http_parser *parser, const char *at,
                size_t len)
{
	HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
	pHttpMsg->mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>(pHttpMsg->body(), std::string(at, len)));
    return (0);
}

int CodecWebSocketPb::OnHeadersComplete(http_parser *parser)
{
	HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
	pHttpMsg->set_body("");
    return (0);
}

int CodecWebSocketPb::OnBody(http_parser *parser, const char *at, size_t len)
{
    HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
	if (pHttpMsg->body().size() > 0)
	{
		pHttpMsg->mutable_body()->append(at, len);
	}
	else
	{
		pHttpMsg->set_body(at, len);
	}
    return (0);
}

int CodecWebSocketPb::OnMessageComplete(http_parser *parser)
{
    HttpMsg* pHttpMsg = (HttpMsg*) parser->data;
    if (0 != parser->status_code)
    {
        pHttpMsg->set_status_code(parser->status_code);
        pHttpMsg->set_type(HTTP_RESPONSE);
    }
    else
    {
        pHttpMsg->set_method(parser->method);
        pHttpMsg->set_type(HTTP_REQUEST);
    }
    pHttpMsg->set_http_major(parser->http_major);
    pHttpMsg->set_http_minor(parser->http_minor);
    if (HTTP_GET == (http_method) pHttpMsg->method())
    {
        pHttpMsg->set_is_decoding(false);
    }
    else
    {
        if (parser->content_length == 0)
        {
            pHttpMsg->set_is_decoding(false);
        }
        else if (parser->content_length == pHttpMsg->body().size())
        {
            pHttpMsg->set_is_decoding(false);
        }
    }
	if (http_should_keep_alive(parser))
	{
		pHttpMsg->set_keep_alive(-1); ;
	}
	else
	{
		pHttpMsg->set_keep_alive(0);
	}
    return (0);
}

int CodecWebSocketPb::OnChunkHeader(http_parser *parser)
{
    return (0);
}

int CodecWebSocketPb::OnChunkComplete(http_parser *parser)
{
    return (0);
}

CodecWebSocketPb::CodecWebSocketPb(util::E_CODEC_TYPE eCodecType,
                const std::string& strKey)
                : ThunderCodec(eCodecType, strKey)
{
}

CodecWebSocketPb::~CodecWebSocketPb()
{
}

E_CODEC_STATUS CodecWebSocketPb::Encode(const MsgHead& oMsgHead,
                const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    /*
     0               1               2               3
     0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
     +-+-+-+-+-------+-+-------------+-------------------------------+
     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
     | |1|2|3|       |K|             |                               |
     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
     |     Extended payload length continued, if payload len == 127  |
     + - - - - - - - - - - - - - - - +-------------------------------+
     |                               |Masking-key, if MASK set to 1  |
     +-------------------------------+-------------------------------+
     | Masking-key (continued)       |          Payload Data         |
     +-------------------------------- - - - - - - - - - - - - - - - +
     :                     Payload Data continued ...                :
     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
     |                     Payload Data continued ...                |
     +---------------------------------------------------------------+
     FIN：1位
                     表示这是消息的最后一帧（结束帧），一个消息由一个或多个数据帧构成。若消息由一帧构成，起始帧即结束帧。
     * */
    /*
     RSV1，RSV2，RSV3：3位
                     如果未定义扩展，各位是0；如果定义了扩展，即为非0值。如果接收的帧此处非0，扩展中却没有该值的定义，那么关闭连接。
     * */
    /*
     OPCODE：4位
                     解释PayloadData，如果接收到未知的opcode，接收端必须关闭连接。
     0x0表示附加数据帧
     0x1表示文本数据帧
     0x2表示二进制数据帧
     0x3-7暂时无定义，为以后的非控制帧保留
     0x8表示连接关闭
     0x9表示ping
     0xA表示pong
     0xB-F暂时无定义，为以后的控制帧保留
     * */
    MsgBody oSwitchMsgBody;
    oSwitchMsgBody.set_body(oMsgBody.body());
    if (oSwitchMsgBody.ByteSize() > 64000000) // pb 最大限制
    {
        LOG4_ERROR("oSwitchMsgBody.ByteSize() > 64000000");
        return (CODEC_STATUS_ERR);
    }
    uint8 ucFirstByte = 0;
    uint8 ucSecondByte = 0;

    LOG4_TRACE("cmd %u, seq %u, oSwitchMsgBody.ByteSize() %u", oMsgHead.cmd(), oMsgHead.seq(),oSwitchMsgBody.ByteSize());
    //在逻辑层处理心跳
	#ifdef HTTP_PING
    if (oMsgHead.msgbody_len() == 0)    // 无包体（心跳包等）
    {
    	int iHadWriteLen = 0;
        ucFirstByte |= WEBSOCKET_FIN;
        if (gc_uiCmdReq & oMsgHead.cmd())   // 发送心跳
        {
            ucFirstByte |= WEBSOCKET_FRAME_PING;
            LOG4_TRACE("cmd %u, seq %u, msgbody_len %u,PING", oMsgHead.cmd(), oMsgHead.seq(),
                                            oMsgHead.msgbody_len());
        }
        else
        {
            ucFirstByte |= WEBSOCKET_FRAME_PONG;
            LOG4_TRACE("cmd %u, seq %u, msgbody_len %u,PONG", oMsgHead.cmd(), oMsgHead.seq(),
                                            oMsgHead.msgbody_len());
        }
        iHadWriteLen += pBuff->Write(&ucFirstByte, 1);
        iHadWriteLen += pBuff->Write(&ucSecondByte, 1);
        LOG4_TRACE("iHadWriteLen = %d",iHadWriteLen);
        return (CODEC_STATUS_OK);
    }
    else
	#endif
    {
    	int iNeedWriteLen = 0;
		int iHadWriteLen = 0;
		int iWriteLen = 0;
        ucFirstByte |= WEBSOCKET_FIN;   //目前没有分帧发送
        ucFirstByte |= WEBSOCKET_FRAME_BINARY;
        tagClientMsgHead stOutMsgHead;
        std::string strCompressData;//压缩数据
        std::string strEncryptData;//加密数据
        const std::string& strPbBody = oSwitchMsgBody.SerializeAsString();//原始数据
        {//payload数据
            stOutMsgHead.version = 1;        // version暂时无用
            stOutMsgHead.encript = static_cast<unsigned char>(oMsgHead.cmd() >> 24);
            stOutMsgHead.cmd = htons(static_cast<unsigned short>(gc_uiCmdBit & oMsgHead.cmd()));
            stOutMsgHead.body_len = 0;
            stOutMsgHead.seq = htonl(oMsgHead.seq());
            stOutMsgHead.checksum = 0;//发送出去的消息不需要校验码，htons(static_cast<unsigned short>(stOutMsgHead.checksum));

            if (gc_uiZipBit & oMsgHead.cmd())   //压缩（zip）
            {
                if (!Zip(strPbBody, strCompressData))
                {
                    LOG4_ERROR("zip error!");
                    return (CODEC_STATUS_ERR);
                }
            }
            else if (gc_uiGzipBit & oMsgHead.cmd())   //压缩（Gzip）
            {
                if (!Gzip(strPbBody, strCompressData))
                {
                    LOG4_ERROR("gzip error!");
                    return (CODEC_STATUS_ERR);
                }
            }
            if (gc_uiRc5Bit & oMsgHead.cmd())   //加密
            {
                if (strCompressData.size() > 0)
                {
                    if (!Rc5Encrypt(strCompressData, strEncryptData))
                    {
                        LOG4_ERROR("Rc5Encrypt error!");
                        return (CODEC_STATUS_ERR);
                    }
                }
                else
                {
                    if (!Rc5Encrypt(strPbBody, strEncryptData))
                    {
                        LOG4_ERROR("Rc5Encrypt error!");
                        return (CODEC_STATUS_ERR);
                    }
                }
            }

            if (strEncryptData.size() > 0)              // 加密后的数据包
            {
                stOutMsgHead.body_len = strEncryptData.size();
                LOG4_TRACE("Encrypt:body_len(%u,%u)",
                        stOutMsgHead.body_len,htonl(static_cast<unsigned int>(stOutMsgHead.body_len)));
            }
            else if (strCompressData.size() > 0)        // 压缩后的数据包
            {
                stOutMsgHead.body_len = strCompressData.size();
                LOG4_TRACE("Compress:body_len(%u,%u)",
                        stOutMsgHead.body_len,htonl(static_cast<unsigned int>(stOutMsgHead.body_len)));
            }
            else    // 不需要压缩加密或无效的压缩或加密算法，打包原数据
            {
                stOutMsgHead.body_len = strPbBody.size();
                LOG4_TRACE("no Encrypt or Compress:body_len(%u,%u)",
                        stOutMsgHead.body_len,htonl(static_cast<unsigned int>(stOutMsgHead.body_len)));
                //no Encrypt or Compress:body_len(587,1258422272)
            }
        }
        int payloadLen = sizeof(stOutMsgHead) + stOutMsgHead.body_len;
        stOutMsgHead.body_len = htonl(static_cast<unsigned int>(stOutMsgHead.body_len));
        if (payloadLen > 65535)
        {
            ucSecondByte |= static_cast<unsigned char>(WEBSOCKET_PAYLOAD_LEN_UINT64) & 0xFF;
            iWriteLen = pBuff->Write(&ucFirstByte, 1);
            iHadWriteLen += iWriteLen;
            iWriteLen = pBuff->Write(&ucSecondByte, 1);
            iHadWriteLen += iWriteLen;
            uint64 ullPayload = payloadLen;
            ullPayload = htonll(ullPayload);
            iWriteLen = pBuff->Write(&ullPayload, sizeof(ullPayload));
            iHadWriteLen += iWriteLen;
            LOG4_TRACE("websocket head iNeedWriteLen = %zu,payloadLen(%d)", 2 + sizeof(ullPayload), payloadLen);
        }
        else if (payloadLen >= 126)
        {
            ucSecondByte |= static_cast<unsigned char>(WEBSOCKET_PAYLOAD_LEN_UINT16) & 0xFF;
            iWriteLen = pBuff->Write(&ucFirstByte, 1);
            iHadWriteLen += iWriteLen;
            iWriteLen = pBuff->Write(&ucSecondByte, 1);
            iHadWriteLen += iWriteLen;
            uint16 unPayload = payloadLen;
            unPayload = htons(unPayload);
            iWriteLen = pBuff->Write(&unPayload, sizeof(unPayload));
            iHadWriteLen += iWriteLen;
            LOG4_TRACE("websocket head iNeedWriteLen = %zu,payloadLen(%d)", 2 + sizeof(unPayload), payloadLen);
        }
        else //if (payloadLen < 126)
        {
            ucSecondByte |= static_cast<unsigned char>(payloadLen) & 0xFF;
            iWriteLen = pBuff->Write(&ucFirstByte, 1);
            iHadWriteLen += iWriteLen;
            iWriteLen = pBuff->Write(&ucSecondByte, 1);
            iHadWriteLen += iWriteLen;
            LOG4_TRACE("websocket head iNeedWriteLen = %d,payloadLen(%d)",2,payloadLen);
        }

        iNeedWriteLen = sizeof(stOutMsgHead);
        iWriteLen = pBuff->Write(&stOutMsgHead, iNeedWriteLen);
        iHadWriteLen += iWriteLen;
        LOG4_TRACE("sizeof(stClientMsgHead) = %zu, iWriteLen = %d",
                        sizeof(stOutMsgHead), iWriteLen);
        if (iWriteLen != iNeedWriteLen)
        {
            LOG4_ERROR("buff write head iWriteLen != sizeof(stClientMsgHead)");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
            return (CODEC_STATUS_ERR);
        }

        if (strEncryptData.size() > 0)              // 加密后的数据包
        {
            iNeedWriteLen = strEncryptData.size();
            iWriteLen = pBuff->Write(strEncryptData.c_str(),
                            strEncryptData.size());
        }
        else if (strCompressData.size() > 0)        // 压缩后的数据包
        {
            iNeedWriteLen = strCompressData.size();
            iWriteLen = pBuff->Write(strCompressData.c_str(),
                            strCompressData.size());
        }
        else    // 无效的压缩或加密算法，打包原数据
        {
            iNeedWriteLen = strPbBody.size();
            iWriteLen = pBuff->Write(strPbBody.c_str(), strPbBody.size());
        }
        if (iWriteLen != iNeedWriteLen)
        {
            LOG4_ERROR("buff iWriteLen != iNeedWriteLen");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
            return (CODEC_STATUS_ERR);
        }
        iHadWriteLen += iWriteLen;
        LOG4_TRACE("body iNeedWriteLen = %d, iWriteLen = %d",
                        iNeedWriteLen, iWriteLen);
        LOG4_TRACE("oMsgBody.ByteSize() = %d,oSwitchMsgBody.ByteSize() = %d,sizeof(stOutMsgHead) = %zu,iHadWriteLen = %d(compress or encrypt maybe)",
                            oMsgBody.ByteSize(),oSwitchMsgBody.ByteSize(),sizeof(stOutMsgHead), iHadWriteLen);
    }
    return (CODEC_STATUS_OK);
}

E_CODEC_STATUS CodecWebSocketPb::Encode(const HttpMsg& oHttpMsg,util::CBuffer* pBuff)
{
    bool boUpgradeWebSocket(false);
    for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
    {
        if ((strcasecmp("Upgrade", iter->first.c_str()) == 0)
                        && (strcasecmp("WebSocket",iter->second.c_str()) == 0))
        {
            boUpgradeWebSocket = true;
        }
    }
    if (boUpgradeWebSocket)//WebSocket 握手
    {
        return EncodeHandShake(oHttpMsg, pBuff);
    }
    else
    {
        return EncodeHttp(oHttpMsg, pBuff);
    }
    return (CODEC_STATUS_OK);
}

E_CODEC_STATUS CodecWebSocketPb::EncodeHandShake(const HttpMsg& oHttpMsg,
                util::CBuffer* pBuff)
{
    LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu, ReadIndex = %zu, WriteIndex = %zu",
                            __FUNCTION__, pBuff->ReadableBytes(),
                            pBuff->GetReadIndex(), pBuff->GetWriteIndex());
    m_mapAddingHttpHeader.clear();
    if (oHttpMsg.http_major() == 0 && oHttpMsg.http_minor() == 0)//1.1
    {
        LOG4_ERROR("miss http version!");
        m_mapAddingHttpHeader.clear();
        return (CODEC_STATUS_ERR);
    }
    int iWriteSize = 0;
    int iHadWriteSize = 0;
    if (oHttpMsg.status_code() == 0)//>= 100
    {
        LOG4_ERROR("miss status code!");
        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
        m_mapAddingHttpHeader.clear();
        return (CODEC_STATUS_ERR);
    }
    iWriteSize = pBuff->Printf("HTTP/%u.%u %u %s\r\n",
                    oHttpMsg.http_major(), oHttpMsg.http_minor(),
                    oHttpMsg.status_code(),
                    status_string(oHttpMsg.status_code()));
    if (iWriteSize < 0)
    {
        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
        m_mapAddingHttpHeader.clear();
        return (CODEC_STATUS_ERR);
    }
    else
    {
        iHadWriteSize += iWriteSize;
    }
    {    //请求字段
        std::unordered_map<std::string, std::string>::iterator h_iter;
        for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
        {
            m_mapAddingHttpHeader.insert(std::make_pair(
                                            iter->first,
                                            iter->second));
        }
        for (h_iter = m_mapAddingHttpHeader.begin();
                        h_iter != m_mapAddingHttpHeader.end(); ++h_iter)
        {
            iWriteSize = pBuff->Printf("%s: %s\r\n", h_iter->first.c_str(),
                            h_iter->second.c_str());
            if (iWriteSize < 0)
            {
                pBuff->SetWriteIndex(
                                pBuff->GetWriteIndex() - iHadWriteSize);
                return (CODEC_STATUS_ERR);
            }
            else
            {
                iHadWriteSize += iWriteSize;
            }
        }
        pBuff->Printf("\r\n");
    }
    iWriteSize = pBuff->WriteByte('\0');
    size_t iWriteIndex = pBuff->GetWriteIndex();
    LOG4_TRACE("%s", pBuff->GetRawReadBuffer());
    pBuff->SetWriteIndex(iWriteIndex - iWriteSize);
    LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu, ReadIndex = %zu, WriteIndex = %zu, iHadWriteSize = %d",
                    __FUNCTION__, pBuff->ReadableBytes(),
                    pBuff->GetReadIndex(), pBuff->GetWriteIndex(),
                    iHadWriteSize);
    m_mapAddingHttpHeader.clear();
    return (CODEC_STATUS_OK);
}

E_CODEC_STATUS CodecWebSocketPb::EncodeHttp(const HttpMsg& oHttpMsg,util::CBuffer* pBuff)
{
    LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu, ReadIndex = %zu, WriteIndex = %zu",
                    __FUNCTION__, pBuff->ReadableBytes(), pBuff->GetReadIndex(),
                    pBuff->GetWriteIndex());
    if (oHttpMsg.http_major() == 0 && oHttpMsg.http_minor() == 0)
    {
        LOG4_ERROR("miss http version!");
        m_mapAddingHttpHeader.clear();
        return (CODEC_STATUS_ERR);
    }

    int iWriteSize = 0;
    int iHadWriteSize = 0;
    if (HTTP_REQUEST == oHttpMsg.type())
    {
        if (oHttpMsg.method() == 0 || oHttpMsg.url().size() == 0)//method >= 0 ,0为 DELETE,不使用该方法
        {
            LOG4_ERROR("miss method or url!");
            m_mapAddingHttpHeader.clear();
            return (CODEC_STATUS_ERR);
        }
        int iPort = 0;
        std::string strHost;
        std::string strPath;
        struct http_parser_url stUrl;
        if (0 == http_parser_parse_url(oHttpMsg.url().c_str(),
                                        oHttpMsg.url().length(), 0, &stUrl))
        {
            if (stUrl.field_set & (1 << UF_PORT))
            {
                iPort = stUrl.port;
            }
            else
            {
                iPort = 80;
            }

            if (stUrl.field_set & (1 << UF_HOST))
            {
                strHost = oHttpMsg.url().substr(stUrl.field_data[UF_HOST].off,
                                stUrl.field_data[UF_HOST].len);
            }

            if (stUrl.field_set & (1 << UF_PATH))
            {
                strPath = oHttpMsg.url().substr(stUrl.field_data[UF_PATH].off,
                                stUrl.field_data[UF_PATH].len);
            }
        }
        else
        {
            LOG4_ERROR("http_parser_parse_url error!");
            m_mapAddingHttpHeader.clear();
            return (CODEC_STATUS_ERR);
        }
        iWriteSize = pBuff->Printf("%s %s HTTP/%u.%u\r\n",
                        http_method_str((http_method) oHttpMsg.method()),
                        oHttpMsg.url().substr(stUrl.field_data[UF_PATH].off,
                                        std::string::npos).c_str(),
                        oHttpMsg.http_major(), oHttpMsg.http_minor());
        if (iWriteSize < 0)
        {
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return (CODEC_STATUS_ERR);
        }
        else
        {
            iHadWriteSize += iWriteSize;
        }
        iWriteSize = pBuff->Printf("Host: %s:%d\r\n", strHost.c_str(), iPort);
        if (iWriteSize < 0)
        {
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return (CODEC_STATUS_ERR);
        }
        else
        {
            iHadWriteSize += iWriteSize;
        }
    }
    else if (HTTP_RESPONSE == oHttpMsg.type())
    {
        if (oHttpMsg.status_code() == 0)//>=100
        {
            LOG4_ERROR("miss status code!");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return (CODEC_STATUS_ERR);
        }
        iWriteSize = pBuff->Printf("HTTP/%u.%u %u %s\r\n",
                        oHttpMsg.http_major(), oHttpMsg.http_minor(),
                        oHttpMsg.status_code(),
                        status_string(oHttpMsg.status_code()));
        if (iWriteSize < 0)
        {
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return (CODEC_STATUS_ERR);
        }
        else
        {
            iHadWriteSize += iWriteSize;
        }
        if (oHttpMsg.http_major() < 1   || (oHttpMsg.http_major() == 1  && oHttpMsg.http_minor() < 1))
        {
            m_mapAddingHttpHeader.insert(std::pair<std::string, std::string>("Connection","close"));
        }
        else
        {
            m_mapAddingHttpHeader.insert(std::pair<std::string, std::string>("Connection","keep-alive"));
        }
        m_mapAddingHttpHeader.insert(std::make_pair("Server", "StarHttp"));
        m_mapAddingHttpHeader.insert(std::make_pair("Content-Type","application/json;charset=UTF-8"));
        m_mapAddingHttpHeader.insert(std::make_pair("Allow", "POST,GET"));
    }
    bool bIsChunked = false;
    bool bIsGzip = false;   // 是否用gizp压缩传输包
    for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
    {
        if (std::string("Content-Length") != iter->first)
        {
            m_mapAddingHttpHeader[iter->first] = iter->second;
        }
        if (std::string("Content-Encoding") == iter->first && std::string("gzip") == iter->second)
        {
            bIsGzip = true;
        }
        if (std::string("Transfer-Encoding") == iter->first && std::string("chunked")  == iter->second)
        {
            bIsChunked = true;
        }
    }
    for (auto h_iter = m_mapAddingHttpHeader.begin();h_iter != m_mapAddingHttpHeader.end(); ++h_iter)
    {
        iWriteSize = pBuff->Printf("%s: %s\r\n", h_iter->first.c_str(),h_iter->second.c_str());
        if (iWriteSize < 0)
        {
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            LOG4_ERROR("Printf error!");
            return (CODEC_STATUS_ERR);
        }
        else
        {
            iHadWriteSize += iWriteSize;
        }
        if (std::string("Transfer-Encoding") == h_iter->first && std::string("chunked") == h_iter->second)
        {
            bIsChunked = true;
        }
    }
    if (oHttpMsg.body().size() > 0)
    {
        std::string strGzipData;
        if (bIsGzip)
        {
            if (!Gzip(oHttpMsg.body(), strGzipData))
            {
                LOG4_ERROR("gzip error!");
                pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                m_mapAddingHttpHeader.clear();
                return (CODEC_STATUS_ERR);
            }
        }

        if (bIsChunked)     // Transfer-Encoding: chunked
        {
            if (oHttpMsg.encoding() == 0)
            {
                iWriteSize = pBuff->Printf("\r\n");
                if (iWriteSize < 0)
                {
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    LOG4_ERROR("Printf error!");
                    return (CODEC_STATUS_ERR);
                }
                else
                {
                    iHadWriteSize += iWriteSize;
                }
            }
            else
            {
                pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                iHadWriteSize = 0;
            }
            if (strGzipData.size() > 0)
            {
                iWriteSize = pBuff->Printf("%x\r\n", strGzipData.size());
                if (iWriteSize < 0)
                {
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    LOG4_ERROR("Printf error!");
                    return (CODEC_STATUS_ERR);
                }
                else
                {
                    iHadWriteSize += iWriteSize;
                }
                iWriteSize = pBuff->Write(strGzipData.c_str(),strGzipData.size());
                if (iWriteSize < 0)
                {
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    LOG4_ERROR("Write error!");
                    return (CODEC_STATUS_ERR);
                }
                else
                {
                    iHadWriteSize += iWriteSize;
                }
            }
            else
            {
                iWriteSize = pBuff->Printf("%x\r\n", oHttpMsg.body().size());
                if (iWriteSize < 0)
                {
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    LOG4_ERROR("Printf error!");
                    return (CODEC_STATUS_ERR);
                }
                else
                {
                    iHadWriteSize += iWriteSize;
                }
                if (oHttpMsg.body().size() == 0)
                {
                    pBuff->Printf("\r\n");
                }
                else
                {
                    iWriteSize = pBuff->Write(oHttpMsg.body().c_str(),
                                    oHttpMsg.body().size());
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Write error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                }
            }
        }
        else    // Content-Length: %u
        {
            if (strGzipData.size() > 0)
            {
                if (strGzipData.size() > 8192)  // 长度太长，使用chunked编码传输
                {
                    bIsChunked = true;
                    iWriteSize = pBuff->Printf( "Transfer-Encoding: chunked\r\n\r\n");
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Printf error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    int iChunkLength = 8192;
                    for (int iPos = 0; iPos < strGzipData.size(); iPos += iChunkLength)
                    {
                        iChunkLength = (iChunkLength  < strGzipData.size() - iPos) ?
                                        iChunkLength :(strGzipData.size() - iPos);
                        iWriteSize = pBuff->Printf("%x\r\n", iChunkLength);
                        if (iWriteSize < 0)
                        {
                            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            LOG4_ERROR("Printf error!");
                            return (CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                        iWriteSize =  pBuff->Printf("%s\r\n",  strGzipData.substr(iPos,iChunkLength).c_str(),
                                                        iChunkLength);
                        if (iWriteSize < 0)
                        {
                            pBuff->SetWriteIndex( pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            LOG4_ERROR("Printf error!");
                            return (CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                    }
                    iWriteSize = pBuff->Printf("0\r\n\r\n");
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Printf error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                }
                else
                {
                    iWriteSize = pBuff->Printf("Content-Length: %u\r\n\r\n",
                                    strGzipData.size());
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Printf error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    iWriteSize = pBuff->Write(strGzipData.c_str(),
                                    strGzipData.size());
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Write error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                }
            }
            else
            {
                if (oHttpMsg.body().size() > 8192)
                {
                    bIsChunked = true;
                    iWriteSize = pBuff->Printf("Transfer-Encoding: chunked\r\n\r\n");
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Printf error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    int iChunkLength = 8192;
                    for (int iPos = 0; iPos < oHttpMsg.body().size(); iPos += iChunkLength)
                    {
                        iChunkLength = (iChunkLength
                                        < oHttpMsg.body().size() - iPos) ?
                                        iChunkLength :
                                        (oHttpMsg.body().size() - iPos);
                        iWriteSize = pBuff->Printf("%x\r\n", iChunkLength);
                        if (iWriteSize < 0)
                        {
                            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            LOG4_ERROR("Printf error!");
                            return (CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                        iWriteSize = pBuff->Printf("%s\r\n",
                                        oHttpMsg.body().substr(iPos,iChunkLength).c_str(),
                                        iChunkLength);
                        if (iWriteSize < 0)
                        {
                            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            LOG4_ERROR("Printf error!");
                            return (CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                    }
                    iWriteSize = pBuff->Printf("0\r\n\r\n");
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Printf error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                }
                else
                {
                    iWriteSize = pBuff->Printf("Content-Length: %u\r\n\r\n",
                                    oHttpMsg.body().size());
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        LOG4_ERROR("Printf error!");
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    iWriteSize = pBuff->Write(oHttpMsg.body().c_str(),
                                    oHttpMsg.body().size());
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(
                                        pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return (CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                }
            }
        }
    }
    else
    {
        if (bIsChunked)
        {
            if (oHttpMsg.encoding() == 0)
            {
                iWriteSize = pBuff->Printf("\r\n");
                if (iWriteSize < 0)
                {
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    LOG4_ERROR("Printf error!");
                    return (CODEC_STATUS_ERR);
                }
                else
                {
                    iHadWriteSize += iWriteSize;
                }
            }
            else
            {
                pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            }
            iWriteSize = pBuff->Printf("0\r\n\r\n");
            if (iWriteSize < 0)
            {
                pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                m_mapAddingHttpHeader.clear();
                LOG4_ERROR("Printf error!");
                return (CODEC_STATUS_ERR);
            }
            else
            {
                iHadWriteSize += iWriteSize;
            }
        }
        else
        {
            iWriteSize = pBuff->Printf("Content-Length: 0\r\n\r\n");
            if (iWriteSize < 0)
            {
                pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                m_mapAddingHttpHeader.clear();
                LOG4_ERROR("Printf error!");
                return (CODEC_STATUS_ERR);
            }
            else
            {
                iHadWriteSize += iWriteSize;
            }
        }
    }
    iWriteSize = pBuff->WriteByte('\0');
    size_t iWriteIndex = pBuff->GetWriteIndex();
    LOG4_TRACE("%s", pBuff->GetRawReadBuffer());
    pBuff->SetWriteIndex(iWriteIndex - iWriteSize);
    LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu, ReadIndex = %zu, WriteIndex = %zu, iHadWriteSize = %d",
                    __FUNCTION__, pBuff->ReadableBytes(), pBuff->GetReadIndex(),
                    pBuff->GetWriteIndex(), iHadWriteSize);
    m_mapAddingHttpHeader.clear();
    return (CODEC_STATUS_OK);
}

//把缓存pBuff 中的内容换序列化到oHttpMsg
E_CODEC_STATUS CodecWebSocketPb::Decode(util::CBuffer* pBuff,
                HttpMsg& oHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (pBuff->ReadableBytes() == 0)
    {
        LOG4_TRACE("no data...");
        return (CODEC_STATUS_PAUSE);
    }
    util::CBuffer oHttpBuff;        // 用于debug输出Decode前的http协议
    oHttpBuff.Write(pBuff->GetRawReadBuffer(), pBuff->ReadableBytes());
    oHttpBuff.WriteByte('\0');
    LOG4_TRACE("%s", oHttpBuff.GetRawReadBuffer());
    //反序列化时构造oHttpMsg
    m_parser_setting.on_message_begin = OnMessageBegin;
    m_parser_setting.on_url = OnUrl;
    m_parser_setting.on_status = OnStatus;
    m_parser_setting.on_header_field = OnHeaderField;
    m_parser_setting.on_header_value = OnHeaderValue;
    m_parser_setting.on_headers_complete = OnHeadersComplete;
    m_parser_setting.on_body = OnBody;
    m_parser_setting.on_message_complete = OnMessageComplete;
    m_parser_setting.on_chunk_header = OnChunkHeader;
    m_parser_setting.on_chunk_complete = OnChunkComplete;
    m_parser.data = &oHttpMsg;
    http_parser_init(&m_parser, HTTP_REQUEST);
    int iReadIdx = pBuff->GetReadIndex();
    size_t uiReadableBytes = pBuff->ReadableBytes();
    size_t uiLen = http_parser_execute(&m_parser, &m_parser_setting,
                    pBuff->GetRawReadBuffer(), uiReadableBytes);
    if (!oHttpMsg.is_decoding())
    {
        if (m_parser.http_errno != HPE_OK)
        {
            LOG4_ERROR("Failed to parse http message for cause:%s",
                            http_errno_name((http_errno )m_parser.http_errno));
            return (CODEC_STATUS_ERR);
        }
        pBuff->AdvanceReadIndex(uiLen);
    }
    else
    {
        LOG4_TRACE("decoding...");
        return (CODEC_STATUS_PAUSE);
    }
    for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
    {
        if (std::string("Content-Encoding") == iter->first && std::string("gzip") == iter->second)
        {
            std::string strData;
            if (Gunzip(oHttpMsg.body(), strData))
            {
                oHttpMsg.set_body(strData);
            }
            else
            {
                LOG4_ERROR("guzip error!");
                return (CODEC_STATUS_ERR);
            }
        }
        else if ("X-Real-IP" == iter->first)
        {
            LOG4_TRACE("X-Real-IP: %s",iter->second.c_str());
        }
        else if ("X-Forwarded-For" == iter->first)
        {
            LOG4_TRACE("X-Forwarded-For: %s",iter->second.c_str());
        }
    }
    pBuff->Compact(8192);
    LOG4_TRACE("%s", ToString(oHttpMsg).c_str());
    return (CODEC_STATUS_OK);
}

const std::string& CodecWebSocketPb::ToString(const HttpMsg& oHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    m_strHttpString.clear();
    char prover[16];
    sprintf(prover, "HTTP/%u.%u", oHttpMsg.http_major(), oHttpMsg.http_minor());

    if (HTTP_REQUEST == oHttpMsg.type())
    {
        if (HTTP_POST == oHttpMsg.method() || HTTP_GET == oHttpMsg.method())
        {
            m_strHttpString += http_method_str((http_method) oHttpMsg.method());
            m_strHttpString += " ";
        }
        m_strHttpString += oHttpMsg.url();
        m_strHttpString += " ";
        m_strHttpString += prover;
        m_strHttpString += "\r\n";
    }
    else
    {
        m_strHttpString += prover;
        if (oHttpMsg.status_code())
        {
            char tmp[10];
            sprintf(tmp, " %u\r\n", oHttpMsg.status_code());
            m_strHttpString += tmp;
        }
    }
    for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
    {
        m_strHttpString += iter->first;
        m_strHttpString += ":";
        m_strHttpString += iter->second;
        m_strHttpString += "\r\n";
    }
    m_strHttpString += "\r\n";

    if (oHttpMsg.body().size())
    {
        m_strHttpString += oHttpMsg.body();
        m_strHttpString += "\r\n\r\n";
    }
    return (m_strHttpString);
}

E_CODEC_STATUS CodecWebSocketPb::Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    if (eConnectStatus_init == pConn->ucConnectStatus)//必须以http请求初始化握手协议，否则不是WebSocketJson协议
    {
        if (pConn->pRecvBuff->ReadableBytes() >= 5)//目前只支持Get post 的初始化握手协议
        {
            //响应的是("HTTP/", 5)
            //请求的是("GET ", 4)  ("POST ", 5)
            const char* pReadAddr = pConn->pRecvBuff->GetRawReadBuffer();//处理http请求
            if ((memcmp(pReadAddr, "GET ", 4) == 0)
                            || memcmp(pReadAddr, "POST ", 5) == 0)
            {
                LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu:%s", __FUNCTION__,
                                pConn->pRecvBuff->ReadableBytes(), pConn->pRecvBuff->ToString().c_str());
                HttpMsg oHttpMsg;
                E_CODEC_STATUS eCodecStatus = Decode(pConn->pRecvBuff.get(), oHttpMsg);
                if (CODEC_STATUS_OK == eCodecStatus)
                {
                    std::string upgrade;
                    //oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("x-cmd", std::to_string(oMsgHead.cmd())));
					//oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("x-seq", std::to_string(oMsgHead.seq())));
					auto iter = oHttpMsg.headers().find("Upgrade");
					if (iter != oHttpMsg.headers().end())
					{
						upgrade = iter->second;
					}
                    if(strcasecmp(upgrade.c_str(),"websocket") == 0)//websocket升级握手消息
                    {
                        pConn->ucConnectStatus = eConnectStatus_ok;//握手协议接收成功
                        oMsgBody.set_body(oHttpMsg.SerializeAsString());
                        oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
                    }
                    else
                    {
                        LOG4_TRACE("%s()　CodecWebSocketPb　need to init connect status with http head Upgrade for websocket",__FUNCTION__);
                        return CODEC_STATUS_ERR;
                    }
                }
                return eCodecStatus;
            }
        }
        LOG4_TRACE("%s()　CodecWebSocketPb　need to init connect status with http request",__FUNCTION__);
        return CODEC_STATUS_PAUSE;
    }
    return Decode(pConn->pRecvBuff.get(),oMsgHead,oMsgBody);
}

E_CODEC_STATUS CodecWebSocketPb::Decode(util::CBuffer* pBuff,MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    if (pBuff->ReadableBytes() >= 2)//处理websocket
    {
        /*
         0               1               2               3
         0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
         +-+-+-+-+-------+-+-------------+-------------------------------+
         |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
         |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
         |N|V|V|V|       |S|             |   (if payload len==126/127)   |
         | |1|2|3|       |K|             |                               |
         +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
         |     Extended payload length continued, if payload len == 127  |
         + - - - - - - - - - - - - - - - +-------------------------------+
         |                               |Masking-key, if MASK set to 1  |
         +-------------------------------+-------------------------------+
         | Masking-key (continued)       |          Payload Data         |
         +-------------------------------- - - - - - - - - - - - - - - - +
         :                     Payload Data continued ...                :
         + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
         |                     Payload Data continued ...                |
         +---------------------------------------------------------------+
         FIN：1位
                         表示这是消息的最后一帧（结束帧），一个消息由一个或多个数据帧构成。若消息由一帧构成，起始帧即结束帧。
         * */
        /*
         RSV1，RSV2，RSV3：3位
                         如果未定义扩展，各位是0；如果定义了扩展，即为非0值。如果接收的帧此处非0，扩展中却没有该值的定义，那么关闭连接。
         * */
        /*
         OPCODE：4位
                         解释PayloadData，如果接收到未知的opcode，接收端必须关闭连接。
         0x0表示附加数据帧
         0x1表示文本数据帧
         0x2表示二进制数据帧
         0x3-7暂时无定义，为以后的非控制帧保留
         0x8表示连接关闭
         0x9表示ping
         0xA表示pong
         0xB-F暂时无定义，为以后的控制帧保留
         * */
        int iReadIdx = pBuff->GetReadIndex();
        uint8 ucFirstByte = 0;
        uint8 ucSecondByte = 0;
        pBuff->Read(&ucFirstByte, 1);
        pBuff->Read(&ucSecondByte, 1);
        if (!(WEBSOCKET_MASK & ucSecondByte))    //0x80 客户端发来的必须包含掩码
        {
            LOG4_ERROR("a masked frame MUST have the field frame-masked set to 1 when client to server!");
            return (CODEC_STATUS_ERR);
        }
        if (0 == (WEBSOCKET_PAYLOAD_LEN & ucSecondByte)) //0x7F，Payload len长度为0，为ping pong 消息
        {
            if (WEBSOCKET_FRAME_PING & ucFirstByte)    //9 ping 消息
            {
                oMsgHead.set_cmd(0);
                oMsgHead.set_seq(0);
                oMsgHead.set_msgbody_len(0);
                LOG4_TRACE("ping msg");
            }
            else
            {
                oMsgHead.set_cmd(1);
                oMsgHead.set_seq(0);
                oMsgHead.set_msgbody_len(0);
                LOG4_TRACE("pong msg");
            }
            return (CODEC_STATUS_PAUSE);//ping pong消息目前服务器不处理
        }
        uint32 uiPayload = 0;
        char szMaskKey[4] = { 0 };
        {//Payloadlen  szMaskKey
            if (WEBSOCKET_PAYLOAD_LEN_UINT64
                            == (WEBSOCKET_PAYLOAD_LEN & ucSecondByte)) //127 == ucSecondByte & 0x7F
            {
                //如果值是127，则后面8个字节的无符号整型数的值是payload的真实长度。注意，网络字节序，需要转换。
                uint64 ullPayload = 0;
                if (pBuff->ReadableBytes() <= 12)   // 8 + 4
                {
                    pBuff->SetReadIndex(iReadIdx);//回退到原来的读坐标
                    return (CODEC_STATUS_PAUSE);
                }
                pBuff->Read(&ullPayload, 8);
                pBuff->Read(&szMaskKey, 4);
                uiPayload = static_cast<uint32>(ntohll(ullPayload));
            }
            else if (WEBSOCKET_PAYLOAD_LEN_UINT16
                            == (WEBSOCKET_PAYLOAD_LEN & ucSecondByte)) //126 == ucSecondByte & 0x7F
            {
                //如果值是126，则后面2个字节的无符号整型数的值是payload的真实长度。注意，网络字节序，需要转换。
                uint16 unPayload = 0;
                if (pBuff->ReadableBytes() <= 6)   // 2 + 4
                {
                    pBuff->SetReadIndex(iReadIdx);
                    return (CODEC_STATUS_PAUSE);
                }
                pBuff->Read(&unPayload, 2);
                pBuff->Read(&szMaskKey, 4);
                uiPayload = static_cast<uint32>(ntohs(unPayload));
            }
            else
            {
                //如果其值在0-125，则是payload的真实长度
                uint8 ucPayload = 0;
                if (pBuff->ReadableBytes() <= 4)   // 4
                {
                    pBuff->SetReadIndex(iReadIdx);
                    return (CODEC_STATUS_PAUSE);
                }
                ucPayload = WEBSOCKET_PAYLOAD_LEN & ucSecondByte; // payload len
                pBuff->Read(&szMaskKey, 4);
                uiPayload = ucPayload;
            }
            if (pBuff->ReadableBytes() < uiPayload)
            {
                pBuff->SetReadIndex(iReadIdx);
                LOG4_TRACE("wait for data.ReadableBytes:%zu,Payload:%u",
                                pBuff->ReadableBytes(),uiPayload);
                return (CODEC_STATUS_PAUSE);
            }
            LOG4_TRACE("uiPayload %u", uiPayload);
        }
        {//payload data
            char cData;
            const char* pRawData = pBuff->GetRawReadBuffer();
            for (int i = pBuff->GetReadIndex(), j = 0; j < uiPayload; ++i, ++j)
            {
                cData = pRawData[j] ^ szMaskKey[j % 4];
                pBuff->SetBytes(&cData, 1, i);
            }
        }
        //处理自定义数据包头（如果有包头的则需要处理）
        size_t uiHeadSize = sizeof(tagClientMsgHead);
        if(pBuff->ReadableBytes() < uiHeadSize)
        {
            std::string strPayload;
            strPayload.resize(pBuff->ReadableBytes());
            strPayload.assign(pBuff->GetRawReadBuffer(), pBuff->ReadableBytes());
            LOG4_TRACE("ReadableBytes(%zu) < uiHeadSize(%zu),strPayload(%s),wait for data",
                            pBuff->ReadableBytes(), uiHeadSize,strPayload.c_str());
            pBuff->SetReadIndex(iReadIdx);
            return (CODEC_STATUS_PAUSE);
        }
        tagClientMsgHead stMsgHead;
        pBuff->Read(&stMsgHead, uiHeadSize);
        LOG4_TRACE("before tranfer:cmd %u, seq %u, len %u, encript %u,pBuff->ReadableBytes() %zu",
                        stMsgHead.cmd, stMsgHead.seq, stMsgHead.body_len,stMsgHead.encript,
                        pBuff->ReadableBytes());
        stMsgHead.cmd = ntohs(stMsgHead.cmd);
        stMsgHead.body_len = ntohl(stMsgHead.body_len);
        stMsgHead.seq = ntohl(stMsgHead.seq);
        stMsgHead.checksum = ntohs(stMsgHead.checksum);
        LOG4_TRACE("after tranfer:cmd %u, seq %u, len %u, encript %u,pBuff->ReadableBytes() %zu,uiPayload(%u)",
                        stMsgHead.cmd, stMsgHead.seq, stMsgHead.body_len,stMsgHead.encript,
                        pBuff->ReadableBytes(),uiPayload);
        oMsgHead.set_cmd(static_cast<unsigned int>(stMsgHead.encript << 24)
                                        | stMsgHead.cmd);
//        oMsgHead.set_msgbody_len(stMsgHead.body_len);
        oMsgHead.set_seq(stMsgHead.seq);
        oMsgHead.set_checksum(stMsgHead.checksum);
        if (uiHeadSize + stMsgHead.body_len > uiPayload)      //
        {
            pBuff->SetReadIndex(pBuff->GetReadIndex() - uiHeadSize);
            std::string strPayload;
            strPayload.resize(pBuff->ReadableBytes());
            strPayload.assign(pBuff->GetRawReadBuffer(), pBuff->ReadableBytes());
            LOG4_TRACE("uiHeadSize(%zu) + stMsgHead.body_len(%u) > uiPayload(%u),strPayload(%s)",
                            uiHeadSize, stMsgHead.body_len, uiPayload,strPayload.c_str());
            pBuff->SetReadIndex(iReadIdx);
            return (CODEC_STATUS_PAUSE);
        }
        if(pBuff->ReadableBytes() < stMsgHead.body_len)
        {
            LOG4_TRACE("ReadableBytes(%zu) < body_len(%u),wait for body data,uiHeadSize(%zu)",
                                        pBuff->ReadableBytes(),stMsgHead.body_len,uiHeadSize);
            pBuff->SetReadIndex(iReadIdx);
            return (CODEC_STATUS_PAUSE);
        }
        if (stMsgHead.encript == 0)       // 未压缩也未加密
        {
            std::string strPbBody;
            strPbBody.resize(stMsgHead.body_len);
            strPbBody.assign(pBuff->GetRawReadBuffer(), stMsgHead.body_len);
            bool bResult = oMsgBody.ParseFromString(strPbBody);
            oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
            if(!bResult)
            {
                LOG4_ERROR("ParseFromString error!");
                return(CODEC_STATUS_ERR);
            }
            pBuff->SkipBytes(stMsgHead.body_len);
            return (CODEC_STATUS_OK);
        }
        else    // 有压缩或加密，先解密再解压，然后用MsgBody反序列化
        {
            std::string strUncompressData;
            std::string strDecryptData;
            if (gc_uiRc5Bit & oMsgHead.cmd())
            {
                std::string strRawData;
                strRawData.assign((const char*) pBuff->GetRawReadBuffer(),
                                stMsgHead.body_len);
                if (!Rc5Decrypt(strRawData, strDecryptData))
                {
                    LOG4_ERROR("Rc5Decrypt error!");
                    return (CODEC_STATUS_ERR);
                }
            }
            if (gc_uiZipBit & oMsgHead.cmd())// 采用gzip压缩
            {
                if (strDecryptData.size() > 0)
                {
                    if (!Unzip(strDecryptData, strUncompressData))
                    {
                        LOG4_ERROR("uncompress error!");
                        return (CODEC_STATUS_ERR);
                    }
                }
                else
                {
                    std::string strRawData;
                    strRawData.assign((const char*) pBuff->GetRawReadBuffer(),
                                    stMsgHead.body_len);
                    if (!Unzip(strRawData, strUncompressData))
                    {
                        LOG4_ERROR("uncompress error!");
                        return (CODEC_STATUS_ERR);
                    }
                }
            }
            else if (gc_uiGzipBit & oMsgHead.cmd())/////< 采用zip压缩
            {
                if (strDecryptData.size() > 0)
                {
                    if (!Gunzip(strDecryptData, strUncompressData))
                    {
                        LOG4_ERROR("uncompress error!");
                        return (CODEC_STATUS_ERR);
                    }
                }
                else
                {
                    std::string strRawData;
                    strRawData.assign((const char*) pBuff->GetRawReadBuffer(),
                                    stMsgHead.body_len);
                    if (!Gunzip(strRawData, strUncompressData))
                    {
                        LOG4_ERROR("uncompress error!");
                        return (CODEC_STATUS_ERR);
                    }
                }
            }

            if (strUncompressData.size() > 0)       // 解压后的数据
            {
                bool bResult = oMsgBody.ParseFromString(strUncompressData);
                oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
                if(!bResult)
                {
                    LOG4_ERROR("ParseFromString error!");
                    return(CODEC_STATUS_ERR);
                }
                pBuff->SkipBytes(stMsgHead.body_len);
            }
            else if (strDecryptData.size() > 0)     // 解密后的数据
            {
                bool bResult = oMsgBody.ParseFromString(strDecryptData);
                oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
                if(!bResult)
                {
                    LOG4_ERROR("ParseFromString error!");
                    return(CODEC_STATUS_ERR);
                }
                pBuff->SkipBytes(stMsgHead.body_len);
            }
            else    // 无效的压缩或解密算法，仍然解析原数据
            {
                std::string strPbBody;
                strPbBody.resize(stMsgHead.body_len);
                strPbBody.assign(pBuff->GetRawReadBuffer(),
                                stMsgHead.body_len);
                bool bResult = oMsgBody.ParseFromString(strPbBody);
                oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
                if(!bResult)
                {
                    LOG4_ERROR("ParseFromString error!");
                    return(CODEC_STATUS_ERR);
                }
                pBuff->SkipBytes(stMsgHead.body_len);
            }
            LOG4_TRACE("decode oMsgBody:%s",oMsgBody.DebugString().c_str());
            return (CODEC_STATUS_OK);
        }
    }
    else
    {
        return (CODEC_STATUS_PAUSE);
    }
}

} /* namespace neb */
