/*******************************************************************************
 * Project:  Net
 * @file     WssCodec.hpp
 * @brief    WSS codec（WebSocket over TLS，复用 CodecWebSocketPb + TLS 传输层）
 ******************************************************************************/
#ifndef SRC_CODEC_WSSCODEC_HPP_
#define SRC_CODEC_WSSCODEC_HPP_

#include <memory>
#include <unordered_map>
#include "CodecWebSocketPb.hpp"

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace net
{

class WssCodec: public CodecWebSocketPb
{
public:
    struct SslConfig
    {
        std::string strServerCertFile;
        std::string strServerKeyFile;
        std::string strServerCaFile;
        bool bServerVerifyClient = false;
    };

    explicit WssCodec(const std::string& strKey = "That's a lizard.");
    virtual ~WssCodec();

    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff) override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody) override;
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn, MsgHead& oMsgHead, MsgBody& oMsgBody) override;

    virtual E_CODEC_STATUS Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff) override;
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg) override;

    E_CODEC_STATUS EncodeToConnection(tagConnectionAttr* pConn, const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff);
    E_CODEC_STATUS EncodeToConnection(tagConnectionAttr* pConn, const HttpMsg& oHttpMsg, util::CBuffer* pBuff);
    void SetConnectionRole(int iFd, bool bServerSide);
    void RemoveConnection(int iFd);
    void SetSslConfig(const SslConfig& oConfig);

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
    SslConfig m_oConfig;
    std::unordered_map<int, bool> m_mapConnRole;
    std::unordered_map<int, std::unique_ptr<TlsConnState>> m_mapTlsState;
};

}  // namespace net

#endif /* SRC_CODEC_WSSCODEC_HPP_ */
