/*******************************************************************************
 * Project:  Net
 * @file     HttpsCodec.hpp
 * @brief    HTTPS codec（当前复用 HttpCodec 编解码，TLS 传输层待扩展）
 ******************************************************************************/
#ifndef SRC_CODEC_HTTPSCODEC_HPP_
#define SRC_CODEC_HTTPSCODEC_HPP_

#include <memory>
#include <unordered_map>
#include "HttpCodec.hpp"

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace net
{

class HttpsCodec: public HttpCodec
{
public:
    struct HttpsConfig
    {
        std::string strServerCertFile;
        std::string strServerKeyFile;
        std::string strServerCaFile;
        bool bServerVerifyClient = false;
        std::string strClientCaFile;
        bool bClientVerifyPeer = false;
    };

    explicit HttpsCodec(const std::string& strKey = "That's a lizard.");
    virtual ~HttpsCodec();

    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff) override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody) override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn, MsgHead& oMsgHead, MsgBody& oMsgBody) override;

    virtual E_CODEC_STATUS Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff) override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg) override;

    E_CODEC_STATUS EncodeToConnection(tagConnectionAttr* pConn, const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff);
    E_CODEC_STATUS EncodeToConnection(tagConnectionAttr* pConn, const HttpMsg& oHttpMsg, util::CBuffer* pBuff);
    void SetConnectionRole(int iFd, bool bServerSide);
    void RemoveConnection(int iFd);
    void SetHttpsConfig(const HttpsConfig& oConfig);

private:
    struct TlsConnState
    {
        SSL_CTX* pCtx = nullptr;
        SSL* pSsl = nullptr;
        bool bServerSide = false;
        bool bHandshakeDone = false;
        util::CBuffer oPlainRecvBuff;
        util::CBuffer oPendingPlainSend;
    };

    bool InitLibrary();
    TlsConnState* EnsureState(tagConnectionAttr* pConn);
    E_CODEC_STATUS DoHandshake(tagConnectionAttr* pConn, TlsConnState* pState, util::CBuffer* pOutBuff);
    E_CODEC_STATUS FeedCipherToSsl(tagConnectionAttr* pConn, TlsConnState* pState);
    E_CODEC_STATUS DrainSslToPlain(TlsConnState* pState);
    E_CODEC_STATUS EncryptPlain(tagConnectionAttr* pConn, TlsConnState* pState, const char* pData, size_t iLen, util::CBuffer* pBuff);
    void DrainOutboundCipher(TlsConnState* pState, util::CBuffer* pOutBuff);
    void DestroyState(TlsConnState& oState);

private:
    bool m_bSslInited = false;
    HttpsConfig m_oConfig;
    std::unordered_map<int, bool> m_mapConnRole;
    std::unordered_map<int, std::unique_ptr<TlsConnState>> m_mapTlsState;
};

}  // namespace net

#endif /* SRC_CODEC_HTTPSCODEC_HPP_ */

