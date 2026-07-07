/*******************************************************************************
 * Project:  Net
 * @file     WssCodec.cpp
 * @brief    WSS codec（当前复用 CodecWebSocketPb 编解码，TLS 传输层待扩展）
 ******************************************************************************/
#include "WssCodec.hpp"
#include <cstring>
#include <cstdlib>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace net
{

WssCodec::WssCodec(const std::string& strKey)
    : CodecWebSocketJson(util::CODEC_WSS, strKey)
{
    InitLibrary();
}

WssCodec::~WssCodec()
{
    for (auto& kv : m_mapTlsState)
    {
        DestroyState(*kv.second);
    }
    m_mapTlsState.clear();
}

E_CODEC_STATUS WssCodec::Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    return CodecWebSocketJson::Encode(oMsgHead, oMsgBody, pBuff);
}

E_CODEC_STATUS WssCodec::Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    return CodecWebSocketJson::Decode(pBuff, oMsgHead, oMsgBody);
}

E_CODEC_STATUS WssCodec::Decode(tagConnectionAttr* pConn, MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    if (pConn == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    TlsConnState* pState = EnsureState(pConn);
    if (pState == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    // 1) 先把网络层收到的 TLS 密文喂给 SSL 引擎（RBIO）。
    E_CODEC_STATUS eFeedStatus = FeedCipherToSsl(pConn, pState);
    if (eFeedStatus == CODEC_STATUS_ERR)
    {
        return CODEC_STATUS_ERR;
    }
    // 2) 握手未完成时优先推进握手；握手报文会被写入发送缓冲。
    E_CODEC_STATUS eHandshakeStatus = DoHandshake(pConn, pState, pConn->pSendBuff.get());
    if (eHandshakeStatus == CODEC_STATUS_ERR)
    {
        return CODEC_STATUS_ERR;
    }
    if (eHandshakeStatus == CODEC_STATUS_PAUSE)
    {
        return CODEC_STATUS_PAUSE;
    }
    // 3) 握手完成后，才消费之前暂存的明文并加密输出。
    if (pState->oPendingPlainSend.ReadableBytes() > 0)
    {
        E_CODEC_STATUS eEncStatus = EncryptPlain(pConn, pState, pState->oPendingPlainSend.GetRawReadBuffer(),
                pState->oPendingPlainSend.ReadableBytes(), pConn->pSendBuff.get());
        if (eEncStatus == CODEC_STATUS_OK)
        {
            pState->oPendingPlainSend.SkipBytes(pState->oPendingPlainSend.ReadableBytes());
        }
        else if (eEncStatus == CODEC_STATUS_ERR)
        {
            return CODEC_STATUS_ERR;
        }
    }
    // 4) 从 SSL 引擎持续提取解密后的明文。
    E_CODEC_STATUS eDrainStatus = DrainSslToPlain(pState);
    if (eDrainStatus == CODEC_STATUS_ERR)
    {
        return CODEC_STATUS_ERR;
    }

    if (pState->oPlainRecvBuff.ReadableBytes() == 0)
    {
        return CODEC_STATUS_PAUSE;
    }

    // 5) 初始连接状态：先走 HTTP Upgrade 握手（明文来自 oPlainRecvBuff，非 pConn->pRecvBuff）。
    if (pConn->ucConnectStatus == eConnectStatus_init)
    {
        if (pState->oPlainRecvBuff.ReadableBytes() < 5)
        {
            return CODEC_STATUS_PAUSE;
        }
        const char* pRaw = pState->oPlainRecvBuff.GetRawReadBuffer();
        if (memcmp(pRaw, "GET ", 4) != 0 && memcmp(pRaw, "POST ", 5) != 0)
        {
            return CODEC_STATUS_ERR;
        }
        HttpMsg oHttpMsg;
        E_CODEC_STATUS eHttpStatus = CodecWebSocketJson::Decode(&pState->oPlainRecvBuff, oHttpMsg);
        if (eHttpStatus != CODEC_STATUS_OK)
        {
            return eHttpStatus;
        }
        auto iter = oHttpMsg.headers().find("Upgrade");
        std::string strUpgrade;
        if (iter != oHttpMsg.headers().end())
        {
            strUpgrade = iter->second;
        }
        if (strcasecmp(strUpgrade.c_str(), "websocket") != 0)
        {
            return CODEC_STATUS_ERR;
        }
        pConn->ucConnectStatus = eConnectStatus_ok;
        oMsgBody.set_body(oHttpMsg.SerializeAsString());
        oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
        return CODEC_STATUS_OK;
    }

    // 6) 握手完成后：走 WebSocket 二进制帧解码。
    return CodecWebSocketJson::Decode(&pState->oPlainRecvBuff, oMsgHead, oMsgBody);
}

E_CODEC_STATUS WssCodec::Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff)
{
    return CodecWebSocketJson::Encode(oHttpMsg, pBuff);
}

E_CODEC_STATUS WssCodec::Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg)
{
    return CodecWebSocketJson::Decode(pBuff, oHttpMsg);
}

E_CODEC_STATUS WssCodec::EncodeToConnection(tagConnectionAttr* pConn, const MsgHead& oMsgHead,
        const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    if (pConn == nullptr || pBuff == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    util::CBuffer oPlain;
    E_CODEC_STATUS eStatus = CodecWebSocketJson::Encode(oMsgHead, oMsgBody, &oPlain);
    if (eStatus != CODEC_STATUS_OK)
    {
        return eStatus;
    }
    TlsConnState* pState = EnsureState(pConn);
    if (pState == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    // 先暂存明文，避免在握手阶段直接丢写。
    pState->oPendingPlainSend.Write(oPlain.GetRawReadBuffer(), oPlain.ReadableBytes());
    E_CODEC_STATUS eHandshakeStatus = DoHandshake(pConn, pState, pBuff);
    if (eHandshakeStatus == CODEC_STATUS_ERR)
    {
        return CODEC_STATUS_ERR;
    }
    // 握手未完成时先返回，等待后续 IO 驱动再次进入。
    if (!pState->bHandshakeDone)
    {
        return CODEC_STATUS_OK;
    }
    if (pState->oPendingPlainSend.ReadableBytes() == 0)
    {
        return CODEC_STATUS_OK;
    }
    const E_CODEC_STATUS eEncStatus = EncryptPlain(pConn, pState, pState->oPendingPlainSend.GetRawReadBuffer(),
            pState->oPendingPlainSend.ReadableBytes(), pBuff);
    if (eEncStatus == CODEC_STATUS_OK)
    {
        pState->oPendingPlainSend.SkipBytes(pState->oPendingPlainSend.ReadableBytes());
    }
    return eEncStatus;
}

E_CODEC_STATUS WssCodec::EncodeToConnection(tagConnectionAttr* pConn, const HttpMsg& oHttpMsg, util::CBuffer* pBuff)
{
    if (pConn == nullptr || pBuff == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    util::CBuffer oPlain;
    E_CODEC_STATUS eStatus = CodecWebSocketJson::Encode(oHttpMsg, &oPlain);
    if (eStatus != CODEC_STATUS_OK)
    {
        return eStatus;
    }
    TlsConnState* pState = EnsureState(pConn);
    if (pState == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    // 与 MsgHead/MsgBody 版本保持一致：握手前统一走明文暂存队列。
    pState->oPendingPlainSend.Write(oPlain.GetRawReadBuffer(), oPlain.ReadableBytes());
    E_CODEC_STATUS eHandshakeStatus = DoHandshake(pConn, pState, pBuff);
    if (eHandshakeStatus == CODEC_STATUS_ERR)
    {
        return CODEC_STATUS_ERR;
    }
    if (!pState->bHandshakeDone)
    {
        return CODEC_STATUS_OK;
    }
    if (pState->oPendingPlainSend.ReadableBytes() == 0)
    {
        return CODEC_STATUS_OK;
    }
    const E_CODEC_STATUS eEncStatus = EncryptPlain(pConn, pState, pState->oPendingPlainSend.GetRawReadBuffer(),
            pState->oPendingPlainSend.ReadableBytes(), pBuff);
    if (eEncStatus == CODEC_STATUS_OK)
    {
        pState->oPendingPlainSend.SkipBytes(pState->oPendingPlainSend.ReadableBytes());
    }
    return eEncStatus;
}

// 记录某个连接 fd 的 TLS 角色（服务端 or 客户端）：
// m_mapConnRole[iFd] = bServerSide;
// 作用是给后续 EnsureState() 创建 SSL 状态时用：
// true：走 TLS_server_method()，并 SSL_set_accept_state()（被动握手）
// false：走 TLS_client_method()，并 SSL_set_connect_state()（主动握手）
// 决定该 fd 后面按“服务端握手”还是“客户端握手”来跑。
void WssCodec::SetConnectionRole(int iFd, bool bServerSide)
{
    m_mapConnRole[iFd] = bServerSide;
}

void WssCodec::RemoveConnection(int iFd)
{
    m_mapConnRole.erase(iFd);
    auto it = m_mapTlsState.find(iFd);
    if (it != m_mapTlsState.end())
    {
        DestroyState(*it->second);
        m_mapTlsState.erase(it);
    }
}

void WssCodec::SetSslConfig(const SslConfig& oConfig)
{
    m_oConfig = oConfig;
}

bool WssCodec::InitLibrary()
{
    if (m_bSslInited)
    {
        return true;
    }
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    m_bSslInited = true;
    return true;
}

WssCodec::TlsConnState* WssCodec::EnsureState(tagConnectionAttr* pConn)
{
    auto it = m_mapTlsState.find(pConn->iFd);
    if (it != m_mapTlsState.end())
    {
        return it->second.get();
    }
    // 每个 fd 独立维护 TLS 状态，角色由 Worker 在建连时注入。
    const bool bServerSide = (m_mapConnRole.find(pConn->iFd) != m_mapConnRole.end())
            ? m_mapConnRole[pConn->iFd] : false;
    auto pNew = std::make_unique<TlsConnState>();
    pNew->bServerSide = bServerSide;
    const SSL_METHOD* pMethod = bServerSide ? TLS_server_method() : TLS_client_method();
    pNew->pCtx = SSL_CTX_new(pMethod);
    if (pNew->pCtx == nullptr)
    {
        return nullptr;
    }
    if (bServerSide)
    {
        if (!m_oConfig.strServerCertFile.empty() && !m_oConfig.strServerKeyFile.empty())
        {
            if (SSL_CTX_use_certificate_file(pNew->pCtx, m_oConfig.strServerCertFile.c_str(), SSL_FILETYPE_PEM) != 1
                    || SSL_CTX_use_PrivateKey_file(pNew->pCtx, m_oConfig.strServerKeyFile.c_str(), SSL_FILETYPE_PEM) != 1)
            {
                LOG4_ERROR("load server cert/key failed(cert=%s,key=%s)", m_oConfig.strServerCertFile.c_str(), m_oConfig.strServerKeyFile.c_str());
            }
            SSL_CTX_set_cipher_list(pNew->pCtx, "DEFAULT:!aNULL:!eNULL:!MD5:!3DES");
        }
        if (!m_oConfig.strServerCaFile.empty())
        {
            SSL_CTX_load_verify_locations(pNew->pCtx, m_oConfig.strServerCaFile.c_str(), nullptr);
        }
        SSL_CTX_set_verify(pNew->pCtx, m_oConfig.bServerVerifyClient ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    }
    pNew->pSsl = SSL_new(pNew->pCtx);
    if (pNew->pSsl == nullptr)
    {
        SSL_CTX_free(pNew->pCtx);
        pNew->pCtx = nullptr;
        return nullptr;
    }
    BIO* pReadBio = BIO_new(BIO_s_mem());
    BIO* pWriteBio = BIO_new(BIO_s_mem());
    if (pReadBio == nullptr || pWriteBio == nullptr)
    {
        if (pReadBio != nullptr)
        {
            BIO_free(pReadBio);
        }
        if (pWriteBio != nullptr)
        {
            BIO_free(pWriteBio);
        }
        SSL_free(pNew->pSsl);
        SSL_CTX_free(pNew->pCtx);
        pNew->pSsl = nullptr;
        pNew->pCtx = nullptr;
        return nullptr;
    }
    SSL_set_bio(pNew->pSsl, pReadBio, pWriteBio);
    if (bServerSide)
    {
        SSL_set_accept_state(pNew->pSsl);
    }
    else
    {
        SSL_set_connect_state(pNew->pSsl);
    }
    auto* pRaw = pNew.get();
    m_mapTlsState.insert(std::make_pair(pConn->iFd, std::move(pNew)));
    return pRaw;
}

E_CODEC_STATUS WssCodec::DoHandshake(tagConnectionAttr* pConn, TlsConnState* pState, util::CBuffer* pOutBuff)
{
    if (pState->bHandshakeDone)
    {
        return CODEC_STATUS_OK;
    }
    // 触发/推进握手；无论成功与否都要先把待发握手密文导出到发送缓冲。
    int iRet = SSL_do_handshake(pState->pSsl);
    DrainOutboundCipher(pState, pOutBuff);
    if (iRet == 1)
    {
        pState->bHandshakeDone = true;
        return CODEC_STATUS_OK;
    }
    const int iErr = SSL_get_error(pState->pSsl, iRet);
    if (iErr == SSL_ERROR_WANT_READ || iErr == SSL_ERROR_WANT_WRITE)
    {
        return CODEC_STATUS_PAUSE;
    }
    LOG4_ERROR("SSL_do_handshake failed, fd=%d ssl_err=%d openssl_err=%lu",
            pConn->iFd, iErr, ERR_get_error());
    return CODEC_STATUS_ERR;
}

E_CODEC_STATUS WssCodec::FeedCipherToSsl(tagConnectionAttr* pConn, TlsConnState* pState)
{
    (void)pState;
    const size_t iReadable = pConn->pRecvBuff->ReadableBytes();
    if (iReadable == 0)
    {
        return CODEC_STATUS_PAUSE;
    }
    // 将连接接收缓冲中的 TLS 记录写入 SSL 的读 BIO。
    BIO* pReadBio = SSL_get_rbio(pState->pSsl);
    const int iWritten = BIO_write(pReadBio, pConn->pRecvBuff->GetRawReadBuffer(), static_cast<int>(iReadable));
    if (iWritten <= 0)
    {
        return CODEC_STATUS_ERR;
    }
    pConn->pRecvBuff->AdvanceReadIndex(iWritten);
    return CODEC_STATUS_OK;
}

E_CODEC_STATUS WssCodec::DrainSslToPlain(TlsConnState* pState)
{
    char szBuf[8192];
    // 持续读取直到 OpenSSL 告知暂时无数据（WANT_READ/WANT_WRITE）。
    for (;;)
    {
        const int iRead = SSL_read(pState->pSsl, szBuf, sizeof(szBuf));
        if (iRead > 0)
        {
            pState->oPlainRecvBuff.Write(szBuf, iRead);
            continue;
        }
        const int iErr = SSL_get_error(pState->pSsl, iRead);
        if (iErr == SSL_ERROR_WANT_READ || iErr == SSL_ERROR_WANT_WRITE)
        {
            return CODEC_STATUS_OK;
        }
        if (iErr == SSL_ERROR_ZERO_RETURN)
        {
            return CODEC_STATUS_ERR;
        }
        return CODEC_STATUS_ERR;
    }
}

E_CODEC_STATUS WssCodec::EncryptPlain(tagConnectionAttr* pConn, TlsConnState* pState, const char* pData, size_t iLen, util::CBuffer* pBuff)
{
    if (iLen == 0)
    {
        return CODEC_STATUS_OK;
    }
    // SSL_write 产出的密文不会直接发网卡，需要从 WBIO 再导出到发送缓冲。
    const int iRet = SSL_write(pState->pSsl, pData, static_cast<int>(iLen));
    DrainOutboundCipher(pState, pBuff);
    if (iRet > 0)
    {
        return CODEC_STATUS_OK;
    }
    const int iErr = SSL_get_error(pState->pSsl, iRet);
    if (iErr == SSL_ERROR_WANT_READ || iErr == SSL_ERROR_WANT_WRITE)
    {
        return CODEC_STATUS_PAUSE;
    }
    LOG4_ERROR("SSL_write failed, fd=%d ssl_err=%d openssl_err=%lu",
            pConn->iFd, iErr, ERR_get_error());
    return CODEC_STATUS_ERR;
}

void WssCodec::DrainOutboundCipher(TlsConnState* pState, util::CBuffer* pOutBuff)
{
    BIO* pWriteBio = SSL_get_wbio(pState->pSsl);
    if (pWriteBio == nullptr || pOutBuff == nullptr)
    {
        return;
    }
    char szBuf[8192];
    // 清空 WBIO 中可读密文，交给上层统一发送。
    for (;;)
    {
        const int iRead = BIO_read(pWriteBio, szBuf, sizeof(szBuf));
        if (iRead <= 0)
        {
            break;
        }
        pOutBuff->Write(szBuf, iRead);
    }
}

void WssCodec::DestroyState(TlsConnState& oState)
{
    if (oState.pSsl != nullptr)
    {
        SSL_free(oState.pSsl);
        oState.pSsl = nullptr;
    }
    if (oState.pCtx != nullptr)
    {
        SSL_CTX_free(oState.pCtx);
        oState.pCtx = nullptr;
    }
}

}  // namespace net

