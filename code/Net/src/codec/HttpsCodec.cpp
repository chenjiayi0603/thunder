/*******************************************************************************
 * Project:  Net
 * @file     HttpsCodec.cpp
 * @brief    HTTPS codec（当前复用 HttpCodec 编解码，TLS 传输层待扩展）
 ******************************************************************************/
#include "HttpsCodec.hpp"
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace net
{

HttpsCodec::HttpsCodec(const std::string& strKey)
    : HttpCodec(util::CODEC_HTTPS, strKey)
{
    InitLibrary();
}

HttpsCodec::~HttpsCodec()
{
    for (auto& kv : m_mapTlsState)
    {
        DestroyState(*kv.second);
    }
    m_mapTlsState.clear();
}

E_CODEC_STATUS HttpsCodec::Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    return HttpCodec::Encode(oMsgHead, oMsgBody, pBuff);
}

E_CODEC_STATUS HttpsCodec::Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    return HttpCodec::Decode(pBuff, oMsgHead, oMsgBody);
}

E_CODEC_STATUS HttpsCodec::Decode(tagConnectionAttr* pConn, MsgHead& oMsgHead, MsgBody& oMsgBody)
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
    // 4) 从 SSL 引擎持续提取解密后的明文，再复用 HttpCodec 解析 HTTP 消息。
    E_CODEC_STATUS eDrainStatus = DrainSslToPlain(pState);
    if (eDrainStatus == CODEC_STATUS_ERR)
    {
        return CODEC_STATUS_ERR;
    }
    return HttpCodec::Decode(&pState->oPlainRecvBuff, oMsgHead, oMsgBody);
}

E_CODEC_STATUS HttpsCodec::Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff)
{
    return HttpCodec::Encode(oHttpMsg, pBuff);
}

E_CODEC_STATUS HttpsCodec::Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg)
{
    return HttpCodec::Decode(pBuff, oHttpMsg);
}

E_CODEC_STATUS HttpsCodec::EncodeToConnection(tagConnectionAttr* pConn, const MsgHead& oMsgHead,
        const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    if (pConn == nullptr || pBuff == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    util::CBuffer oPlain;
    E_CODEC_STATUS eStatus = HttpCodec::Encode(oMsgHead, oMsgBody, &oPlain);
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

E_CODEC_STATUS HttpsCodec::EncodeToConnection(tagConnectionAttr* pConn, const HttpMsg& oHttpMsg, util::CBuffer* pBuff)
{
    if (pConn == nullptr || pBuff == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    util::CBuffer oPlain;
    E_CODEC_STATUS eStatus = HttpCodec::Encode(oHttpMsg, &oPlain);
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

void HttpsCodec::SetConnectionRole(int iFd, bool bServerSide)
{
    m_mapConnRole[iFd] = bServerSide;
}

void HttpsCodec::RemoveConnection(int iFd)
{
    m_mapConnRole.erase(iFd);
    auto it = m_mapTlsState.find(iFd);
    if (it != m_mapTlsState.end())
    {
        DestroyState(*it->second);
        m_mapTlsState.erase(it);
    }
}

void HttpsCodec::SetHttpsConfig(const HttpsConfig& oConfig)
{
    m_oConfig = oConfig;
}

bool HttpsCodec::InitLibrary()
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

HttpsCodec::TlsConnState* HttpsCodec::EnsureState(tagConnectionAttr* pConn)
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
        }
        if (!m_oConfig.strServerCaFile.empty())
        {
            SSL_CTX_load_verify_locations(pNew->pCtx, m_oConfig.strServerCaFile.c_str(), nullptr);
        }
        SSL_CTX_set_verify(pNew->pCtx, m_oConfig.bServerVerifyClient ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    }
    else
    {
        if (!m_oConfig.strClientCaFile.empty())
        {
            SSL_CTX_load_verify_locations(pNew->pCtx, m_oConfig.strClientCaFile.c_str(), nullptr);
        }
        SSL_CTX_set_verify(pNew->pCtx, m_oConfig.bClientVerifyPeer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    }
    pNew->pSsl = SSL_new(pNew->pCtx);
    if (pNew->pSsl == nullptr)
    {
        SSL_CTX_free(pNew->pCtx);
        pNew->pCtx = nullptr;
        return nullptr;
    }
    // 采用内存 BIO：网络层与 OpenSSL 通过缓冲区解耦，便于事件驱动收发。
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

E_CODEC_STATUS HttpsCodec::DoHandshake(tagConnectionAttr* pConn, TlsConnState* pState, util::CBuffer* pOutBuff)
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

E_CODEC_STATUS HttpsCodec::FeedCipherToSsl(tagConnectionAttr* pConn, TlsConnState* pState)
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

E_CODEC_STATUS HttpsCodec::DrainSslToPlain(TlsConnState* pState)
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

E_CODEC_STATUS HttpsCodec::EncryptPlain(tagConnectionAttr* pConn, TlsConnState* pState, const char* pData, size_t iLen, util::CBuffer* pBuff)
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

void HttpsCodec::DrainOutboundCipher(TlsConnState* pState, util::CBuffer* pOutBuff)
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

void HttpsCodec::DestroyState(TlsConnState& oState)
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

