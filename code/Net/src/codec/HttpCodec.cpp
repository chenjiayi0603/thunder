/*******************************************************************************
 * Project:  Net
 * @file     HttpCodec.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年10月6日
 * @note
 * Modify history:
 ******************************************************************************/
#include "HttpCodec.hpp"
#include "codec/HttpFastCodec.hpp"
#include "codec/StringCoder.hpp"
#include "CodecCommon.hpp"

namespace net
{

HttpCodec::HttpCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey)
    : ThunderCodec(eCodecType, strKey)
{
}

HttpCodec::~HttpCodec()
{
}

E_CODEC_STATUS HttpCodec::Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    HttpMsg oHttpMsg;
    if (oHttpMsg.ParseFromString(oMsgBody.body()))
    {
		#ifdef HTTP_CMD
    	if (oMsgHead.cmd())
    	{
    		oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("x-cmd", std::to_string(oMsgHead.cmd())));
    	}
    	if (oMsgHead.seq())
    	{
    		oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("x-seq", std::to_string(oMsgHead.seq())));
    	}
        #endif
        return(Encode(oHttpMsg, pBuff));
    }
    else
    {
        LOG4_ERROR("oHttpMsg.ParseFromString() error!");
        return(CODEC_STATUS_ERR);
    }
}

E_CODEC_STATUS HttpCodec::Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    // 获取或创建 per-connection Arena 上下文 (首次请求时分配)
    auto* ctx = static_cast<HttpConnContext*>(pConn->pProtoCtx);
    if (!ctx)
    {
        ctx = new HttpConnContext();
        pConn->pProtoCtx = ctx;
    }

    if (eConnectStatus_init == pConn->ucConnectStatus)
    {
        // Do not pass full recv buffer into LOG4 %s: binary/NULs and large snapshots can corrupt log4cplus buffers (seen as mangled __FILE__ on next line).
        LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu (init http, body omitted from log)", __FUNCTION__,
                        pConn->pRecvBuff->ReadableBytes());
        // 从 Arena 分配 HttpMsg (零 malloc)
        auto* pHttpMsg = google::protobuf::Arena::Create<HttpMsg>(&ctx->arena);
        E_CODEC_STATUS eCodecStatus = Decode(pConn->pRecvBuff.get(), *pHttpMsg);
        if (CODEC_STATUS_OK == eCodecStatus)//第一个消息解析成功则连接初始化成功
        {
            pConn->ucConnectStatus = eConnectStatus_ok;//协议接收成功，表示连接成功
            oMsgBody.set_body(pHttpMsg->SerializeAsString());
            oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
            ctx->arena.Reset();  // Arena 游标归零, block 复用
            return eCodecStatus;
        }
        else if (CODEC_STATUS_ERR == eCodecStatus)
        {
            LOG4_TRACE("%s()　HttpCodec　need to init connect status with http request",__FUNCTION__);
        }
        ctx->arena.Reset();  // 解析失败也 Reset, 避免死空间累积
        return eCodecStatus;
    }

    // 已初始化的连接: 从 Arena 分配 HttpMsg
    {
        auto* pHttpMsg = google::protobuf::Arena::Create<HttpMsg>(&ctx->arena);
        E_CODEC_STATUS eCodecStatus = Decode(pConn->pRecvBuff.get(), *pHttpMsg);
        if (eCodecStatus == CODEC_STATUS_OK)
        {
            oMsgBody.set_body(pHttpMsg->SerializeAsString());
            oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
        }
        ctx->arena.Reset();  // 每个请求结束 Reset
        return eCodecStatus;
    }
}

E_CODEC_STATUS HttpCodec::Decode(util::CBuffer* pBuff,MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    LOG4_TRACE("%s() readable=%u", __FUNCTION__, pBuff ? static_cast<unsigned>(pBuff->ReadableBytes()) : 0u);
    if (pBuff->ReadableBytes() == 0)
    {
        LOG4_TRACE("no data...");
        return(CODEC_STATUS_PAUSE);
    }
    HttpMsg oHttpMsg;
    E_CODEC_STATUS eCodecStatus = Decode(pBuff, oHttpMsg);
    if (CODEC_STATUS_OK == eCodecStatus)
    {
        oMsgBody.set_body(oHttpMsg.SerializeAsString());
        oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
		#ifdef HTTP_CMD
        for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
        {
            if (std::string("x-cmd") == iter->first
                            || std::string("x-CMD") == iter->first
                            || std::string("x-Cmd") == iter->first)
            {
                oMsgHead.set_cmd(atoi(iter->second.c_str()));
            }
            else if (std::string("x-seq") == iter->first
                            || std::string("x-SEQ") == iter->first
                            || std::string("x-Seq") == iter->first)
            {
                oMsgHead.set_seq(strtoul(iter->second.c_str(), NULL, 10));
            }
		}
		#endif
    }
    return(eCodecStatus);
}

E_CODEC_STATUS HttpCodec::Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff)
{
    LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu, ReadIndex = %zu, WriteIndex = %zu",
                    __FUNCTION__, pBuff->ReadableBytes(), pBuff->GetReadIndex(), pBuff->GetWriteIndex());
    if (oHttpMsg.ByteSize() > 64000000) // pb 最大限制
    {
        LOG4_ERROR("oHttpMsg.ByteSize() > 64000000");
        return (CODEC_STATUS_ERR);
    }
    if (oHttpMsg.http_major() == 0 && oHttpMsg.http_minor() == 0)
    {
        LOG4_ERROR("miss http version!");
        m_mapAddingHttpHeader.clear();
        return(CODEC_STATUS_ERR);
    }

    int iWriteSize = 0;
    int iHadWriteSize = 0;
    if (HTTP_REQUEST == oHttpMsg.type())
    {
        if (oHttpMsg.method() == 0 || oHttpMsg.url().size() == 0)//oHttpMsg.has_method() == 0为 DELETE .不能使用DELETE
        {
            LOG4_ERROR("miss method or url!");
            m_mapAddingHttpHeader.clear();
            return(CODEC_STATUS_ERR);
		}
        int iPort = 0;
        std::string strHost;
        std::string strPath;
        struct http_parser_url stUrl;
        if(0 == http_parser_parse_url(oHttpMsg.url().c_str(), oHttpMsg.url().length(), 0, &stUrl))
        {
            if(stUrl.field_set & (1 << UF_PORT))
            {
                iPort = stUrl.port;
            }
            else
            {
                iPort = 80;
            }

            if(stUrl.field_set & (1 << UF_HOST) )
            {
                strHost = oHttpMsg.url().substr(stUrl.field_data[UF_HOST].off, stUrl.field_data[UF_HOST].len);
            }

            if(stUrl.field_set & (1 << UF_PATH))
            {
                strPath = oHttpMsg.url().substr(stUrl.field_data[UF_PATH].off, stUrl.field_data[UF_PATH].len);
            }

            if (iPort == 80)
			{
				std::string strSchema = oHttpMsg.url().substr(0, oHttpMsg.url().find_first_of(':'));
				std::transform(strSchema.begin(), strSchema.end(), strSchema.begin(), [](unsigned char c) -> unsigned char { return std::tolower(c); });
				if (strSchema == std::string("https"))
				{
					iPort = 443;
				}
			}
		}
        else
        {
            LOG4_ERROR("http_parser_parse_url error!");
            m_mapAddingHttpHeader.clear();
            return(CODEC_STATUS_ERR);
		}
        if (strPath.size() > 0)
        {
            iWriteSize = pBuff->Printf("%s %s HTTP/%u.%u\r\n", http_method_str((http_method)oHttpMsg.method()),
                                    strPath.c_str(),oHttpMsg.http_major(), oHttpMsg.http_minor());
		}
        else
        {
            iWriteSize = pBuff->Printf("%s %s HTTP/%u.%u\r\n", http_method_str((http_method)oHttpMsg.method()),
                                    oHttpMsg.url().c_str(),oHttpMsg.http_major(), oHttpMsg.http_minor());
		}
        if (iWriteSize < 0)
        {
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return(CODEC_STATUS_ERR);
		}
        else
        {
            iHadWriteSize += iWriteSize;
		}
        iWriteSize = pBuff->Printf("Host: %s:%d\r\n", strHost.c_str(), iPort);
        if (iWriteSize < 0)
        {
            LOG4_ERROR("pBuff->Printf error!");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return(CODEC_STATUS_ERR);
		}
        else
        {
            iHadWriteSize += iWriteSize;
		}
        {//添加头部
        	for (auto c_iter = oHttpMsg.headers().begin();c_iter != oHttpMsg.headers().end(); ++c_iter)
			{
        		m_mapAddingHttpHeader.insert(std::make_pair(c_iter->first, c_iter->second));
			}
		}
    }
    else if (HTTP_RESPONSE == oHttpMsg.type())
    {
        if (oHttpMsg.status_code() == 0)//>=100
        {
            LOG4_ERROR("miss status code!");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return(CODEC_STATUS_ERR);
		}
        iWriteSize = pBuff->Printf("HTTP/%u.%u %u %s\r\n", oHttpMsg.http_major(), oHttpMsg.http_minor(),
                        oHttpMsg.status_code(), status_string(oHttpMsg.status_code()));
        if (iWriteSize < 0)
        {
            LOG4_ERROR("pBuff->Printf error!");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return(CODEC_STATUS_ERR);
		}
        else
        {
            iHadWriteSize += iWriteSize;
		}
        if (oHttpMsg.http_major() < 1 || (oHttpMsg.http_major() == 1 && oHttpMsg.http_minor() < 1))
        {
            m_mapAddingHttpHeader.insert(std::pair<std::string, std::string>("Connection", "close"));
		}
        else
        {
            m_mapAddingHttpHeader.insert(std::pair<std::string, std::string>("Connection", "keep-alive"));
		}
//        m_mapAddingHttpHeader.insert(std::make_pair("Server", "StarHttp"));
        // 先合并业务层响应头；默认 JSON 的 Content-Type 仅在没有显式设置时加入（否则 insert 不覆盖，会盖住 text/html 等）
        for (auto c_iter = oHttpMsg.headers().begin(); c_iter != oHttpMsg.headers().end(); ++c_iter)
        {
            m_mapAddingHttpHeader.insert(std::make_pair(c_iter->first, c_iter->second));
		}
        if (m_mapAddingHttpHeader.find("Content-Type") == m_mapAddingHttpHeader.end())
        {
            m_mapAddingHttpHeader.insert(std::make_pair("Content-Type", "application/json;charset=UTF-8"));
		}
    }
    bool bIsChunked = false;
    bool bIsGzip = false;   // 是否用gizp压缩传输包
    std::unordered_map<std::string, std::string>::iterator h_iter;
    for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
    {
        if (std::string("Content-Length") == iter->first)
        {
            h_iter = m_mapAddingHttpHeader.find(iter->first);
            if (h_iter == m_mapAddingHttpHeader.end())
            {
                m_mapAddingHttpHeader.insert(std::make_pair(iter->first, iter->second));
            }
            else
            {
                h_iter->second = iter->second;
            }
		}
        if (std::string("Content-Encoding") == iter->first && std::string("gzip") == iter->second)
        {
            bIsGzip = true;
		}
        if (std::string("Transfer-Encoding") == iter->first && std::string("chunked") == iter->first)
        {
            bIsChunked = true;
		}
    }
    // === Fast Encode Path ===
    // 常见场景 (HTTP/1.1 200, 无 gzip/chunked, 默认 headers) 用预编译模板
    if (!bIsGzip && !bIsChunked
        && TryFastEncodeHttpResponse(oHttpMsg, m_mapAddingHttpHeader, pBuff, iHadWriteSize))
    {
        m_mapAddingHttpHeader.clear();
        return(CODEC_STATUS_OK);
    }
    // === Fast Encode Path End ===

    for (h_iter = m_mapAddingHttpHeader.begin(); h_iter != m_mapAddingHttpHeader.end(); ++h_iter)
    {
        iWriteSize = pBuff->Printf("%s: %s\r\n", h_iter->first.c_str(), h_iter->second.c_str());
        if (iWriteSize < 0)
        {
            LOG4_ERROR("pBuff->Printf error!");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
            m_mapAddingHttpHeader.clear();
            return(CODEC_STATUS_ERR);
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
                return(CODEC_STATUS_ERR);
            }
		}

        if (bIsChunked)     // Transfer-Encoding: chunked
        {
            if (oHttpMsg.encoding() == 0)
            {
                iWriteSize = pBuff->Printf("\r\n");
                if (iWriteSize < 0)
                {
                    LOG4_ERROR("pBuff->Printf error!");
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    return(CODEC_STATUS_ERR);
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
                    LOG4_ERROR("pBuff->Printf error!");
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    return(CODEC_STATUS_ERR);
                }
                else
                {
                    iHadWriteSize += iWriteSize;
                }
                iWriteSize = pBuff->Write(strGzipData.c_str(), strGzipData.size());
                if (iWriteSize < 0)
                {
                    LOG4_ERROR("pBuff->Printf error!");
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    return(CODEC_STATUS_ERR);
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
                    LOG4_ERROR("pBuff->Printf error!");
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    return(CODEC_STATUS_ERR);
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
                    iWriteSize = pBuff->Write(oHttpMsg.body().c_str(), oHttpMsg.body().size());
                    if (iWriteSize < 0)
                    {
                        LOG4_ERROR("pBuff->Write error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
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
                    iWriteSize = pBuff->Printf("Transfer-Encoding: chunked\r\n\r\n");
                    if (iWriteSize < 0)
                    {
                        LOG4_ERROR("pBuff->Printf error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    int iChunkLength = 8192;
                    for (int iPos = 0; iPos < strGzipData.size(); iPos += iChunkLength)
                    {
                        iChunkLength = (iChunkLength < strGzipData.size() - iPos) ? iChunkLength : (strGzipData.size() - iPos);
                        iWriteSize = pBuff->Printf("%x\r\n", iChunkLength);
                        if (iWriteSize < 0)
                        {
                            LOG4_ERROR("pBuff->Printf error!");
                            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            return(CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                        iWriteSize = pBuff->Printf("%s\r\n", strGzipData.substr(iPos, iChunkLength).c_str(), iChunkLength);
                        if (iWriteSize < 0)
                        {
                            LOG4_ERROR("pBuff->Printf error!");
                            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            return(CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                    }
                    iWriteSize = pBuff->Printf("0\r\n\r\n");
                    if (iWriteSize < 0)
                    {
                        LOG4_ERROR("pBuff->Printf error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                }
                else
                {
                    iWriteSize = pBuff->Printf("Content-Length: %u\r\n\r\n", strGzipData.size());
                    if (iWriteSize < 0)
                    {
                        LOG4_ERROR("pBuff->Printf error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    iWriteSize = pBuff->Write(strGzipData.c_str(), strGzipData.size());
                    if (iWriteSize < 0)
                    {
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
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
                        LOG4_ERROR("pBuff->Printf error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    int iChunkLength = 8192;
                    for (int iPos = 0; iPos < oHttpMsg.body().size(); iPos += iChunkLength)
                    {
                        iChunkLength = (iChunkLength < oHttpMsg.body().size() - iPos) ? iChunkLength : (oHttpMsg.body().size() - iPos);
                        iWriteSize = pBuff->Printf("%x\r\n", iChunkLength);
                        if (iWriteSize < 0)
                        {
                            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            return(CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                        iWriteSize = pBuff->Printf("%s\r\n", oHttpMsg.body().substr(iPos, iChunkLength).c_str(), iChunkLength);
                        if (iWriteSize < 0)
                        {
                            LOG4_ERROR("pBuff->Printf error!");
                            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                            m_mapAddingHttpHeader.clear();
                            return(CODEC_STATUS_ERR);
                        }
                        else
                        {
                            iHadWriteSize += iWriteSize;
                        }
                    }
                    iWriteSize = pBuff->Printf("0\r\n\r\n");
                    if (iWriteSize < 0)
                    {
                        LOG4_ERROR("pBuff->Printf error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                }
                else
                {
                    iWriteSize = pBuff->Printf("Content-Length: %u\r\n\r\n", oHttpMsg.body().size());
                    if (iWriteSize < 0)
                    {
                        LOG4_ERROR("pBuff->Printf error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
                    }
                    else
                    {
                        iHadWriteSize += iWriteSize;
                    }
                    iWriteSize = pBuff->Write(oHttpMsg.body().c_str(), oHttpMsg.body().size());
                    if (iWriteSize < 0)
                    {
                        LOG4_ERROR("pBuff->Write error!");
                        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                        m_mapAddingHttpHeader.clear();
                        return(CODEC_STATUS_ERR);
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
                    LOG4_ERROR("pBuff->Printf error!");
                    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                    m_mapAddingHttpHeader.clear();
                    return(CODEC_STATUS_ERR);
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
                LOG4_ERROR("pBuff->Printf error!");
                pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                m_mapAddingHttpHeader.clear();
                return(CODEC_STATUS_ERR);
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
                LOG4_ERROR("pBuff->Printf error!");
                pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
                m_mapAddingHttpHeader.clear();
                return(CODEC_STATUS_ERR);
            }
            else
            {
                iHadWriteSize += iWriteSize;
            }
		}
    }
    LOG4_TRACE("%s() pBuff->ReadableBytes() = %zu, ReadIndex = %zu, WriteIndex = %zu, iHadWriteSize = %d",
                    __FUNCTION__, pBuff->ReadableBytes(), pBuff->GetReadIndex(), pBuff->GetWriteIndex(), iHadWriteSize);
    m_mapAddingHttpHeader.clear();
    return(CODEC_STATUS_OK);
}

E_CODEC_STATUS HttpCodec::Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (pBuff->ReadableBytes() == 0)
    {
        LOG4_TRACE("no data...");
        return(CODEC_STATUS_PAUSE);
    }

    // === picohttpparser Fast-Path ===
    // phr_parse_request 覆盖全部 HTTP/1.1 (含 chunked), 替代 http_parser
    size_t consumed = 0;
    if (TryFastDecodeHttpRequest(pBuff->GetRawReadBuffer(), pBuff->ReadableBytes(),
            oHttpMsg, consumed)
        || TryFastDecodeHttpResponse(pBuff->GetRawReadBuffer(), pBuff->ReadableBytes(),
            oHttpMsg, consumed))
    {
        pBuff->AdvanceReadIndex(consumed);
        auto iter = oHttpMsg.headers().find("Content-Encoding");
        if (iter != oHttpMsg.headers().end() && iter->second == "gzip")
        {
            std::string strData;
            if (Gunzip(oHttpMsg.body(), strData))
                oHttpMsg.set_body(strData);
            else
            {
                LOG4_ERROR("gunzip error!");
                return CODEC_STATUS_ERR;
            }
        }
        return CODEC_STATUS_OK;
    }

    // pico 解析失败 — 等待更多数据
    LOG4_TRACE("pico parse incomplete, waiting more data");
    return CODEC_STATUS_PAUSE;}

void HttpCodec::AddHttpHeader(const std::string& strHeaderName, const std::string& strHeaderValue)
{
    m_mapAddingHttpHeader.insert(std::make_pair(strHeaderName, strHeaderValue));
}

const std::string& HttpCodec::ToString(const HttpMsg& oHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    m_strHttpString.clear();
    char prover[16];
    snprintf(prover, sizeof(prover), "HTTP/%u.%u", oHttpMsg.http_major(), oHttpMsg.http_minor());

    if (HTTP_REQUEST == oHttpMsg.type())
    {
        if (oHttpMsg.method() > 0)
        {
            m_strHttpString += http_method_str((http_method)oHttpMsg.method());
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
        if (oHttpMsg.status_code() > 0)
        {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), " %u\r\n", oHttpMsg.status_code());
            m_strHttpString += tmp;
		}
    }
    for (auto iter = oHttpMsg.headers().end(); iter != oHttpMsg.headers().end(); ++iter)
    {
        m_strHttpString += iter->first;
        m_strHttpString += ":";
        m_strHttpString += iter->second;
        m_strHttpString += "\r\n";
    }
    m_strHttpString += "\r\n";

    if (oHttpMsg.body().size() > 0)
    {
        m_strHttpString += oHttpMsg.body();
        m_strHttpString += "\r\n\r\n";
    }
    return(m_strHttpString);
}

} /* namespace net */
