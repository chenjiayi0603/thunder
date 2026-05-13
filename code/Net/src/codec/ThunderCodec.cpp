/*******************************************************************************
 * Project:  Net
 * @file     ThunderCodec.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年10月6日
 * @note
 * Modify history:
 ******************************************************************************/

#include "cryptopp/default.h"
#include "cryptopp/cryptlib.h"
#include "cryptopp/aes.h"
#include "cryptopp/gzip.h"
#include "util/encrypt/rc5.h"

#include "ThunderCodec.hpp"

#include <vector>

namespace net
{

ThunderCodec::ThunderCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey)
    : util::CStreamCodec(eCodecType), m_strKey(strKey)//, m_oAes(strKey)
{
}

ThunderCodec::~ThunderCodec()
{
}

bool ThunderCodec::Zip(const std::string& strSrc, std::string& strDest)
{
    int iErr = Z_OK;
    uLongf ulInLen = strSrc.size();
    uLongf ulOutBuffLen = strSrc.size();
    uLongf ulOutlen = 0;
    z_stream d_stream = { 0 }; /* decompression stream */
    std::vector<Bytef> vecInBuff(ulInLen);
    std::vector<Bytef> vecOutBuff(ulOutBuffLen);
    Bytef* pInBuff = vecInBuff.data();
    Bytef* pOutBuff = vecOutBuff.data();
    d_stream.zalloc = reinterpret_cast<alloc_func>(Z_NULL);
    d_stream.zfree = reinterpret_cast<free_func>(Z_NULL);
    d_stream.opaque = reinterpret_cast<voidpf>(Z_NULL);
    d_stream.next_in = pInBuff;
    d_stream.avail_in = strSrc.size();
    d_stream.next_out = pOutBuff;
    d_stream.avail_out = ulOutBuffLen;

    if (deflateInit(&d_stream, Z_DEFAULT_COMPRESSION) != Z_OK)
    {
        return false;
    }

    memcpy(pInBuff, strSrc.c_str(), ulInLen);
    while (d_stream.avail_in != 0 && d_stream.total_out < ulOutBuffLen)
    {
        iErr = deflate(&d_stream, Z_NO_FLUSH);
        if (iErr != Z_OK)
        {
            iErr = deflateEnd(&d_stream);
            return (false);
        }
    }

    if (d_stream.avail_in != 0) /* 输出缓冲区不够 */
    {
        iErr = deflateEnd(&d_stream);
        return (false);
    }

    iErr = deflate(&d_stream, Z_FINISH);

    iErr = deflateEnd(&d_stream);
    if (iErr != Z_OK)
    {
        return (false);
    }

    strDest.assign(reinterpret_cast<const char*>(pOutBuff), d_stream.total_out);
    return (true);
}

bool ThunderCodec::Unzip(const std::string& strSrc, std::string& strDest)
{
    int iErr = Z_OK;
    uLongf ulInLen = strSrc.size();
    uLongf ulOutBuffLen = 1024000; /* PB的Message最大支持1MB */
    uLongf ulOutlen = 0;
    z_stream d_stream = { 0 }; /* decompression stream */
    std::vector<Bytef> vecInBuff(ulInLen);
    std::vector<Bytef> vecOutBuff(ulOutBuffLen);
    Bytef* pInBuff = vecInBuff.data();
    Bytef* pOutBuff = vecOutBuff.data();
    d_stream.zalloc = reinterpret_cast<alloc_func>(Z_NULL);
    d_stream.zfree = reinterpret_cast<free_func>(Z_NULL);
    d_stream.opaque = reinterpret_cast<voidpf>(Z_NULL);
    d_stream.next_in = pInBuff;
    d_stream.avail_in = 0;
    d_stream.avail_out = ulOutBuffLen;
    d_stream.next_out = pOutBuff;

    if (inflateInit(&d_stream) != Z_OK)
    {
        return false;
    }

    memcpy(pInBuff, strSrc.c_str(), ulInLen);
    while (d_stream.total_out < ulOutBuffLen && d_stream.total_in < ulInLen)
    {
        /* d_stream.avail_in = d_stream.avail_out = 1; // force small buffers */
        iErr = inflate(&d_stream, Z_NO_FLUSH);
        if (iErr == Z_STREAM_END)
        {
            break;
        }
        if (iErr != Z_OK)
        {
            iErr = inflateEnd(&d_stream);
            return (false);
        }
    }

    iErr = inflateEnd(&d_stream);
    if (iErr != Z_OK)
    {
        return (false);
    }

    strDest.assign(reinterpret_cast<const char*>(pOutBuff), d_stream.total_out);
    return (true);
}

bool ThunderCodec::Gzip(const std::string& strSrc, std::string& strDest)
{
    try
    {
        CryptoPP::Gzip oGzipper;
        oGzipper.Put(reinterpret_cast<const unsigned char*>(strSrc.c_str()), strSrc.size());
        oGzipper.MessageEnd();

        CryptoPP::word64 avail = oGzipper.MaxRetrievable();
        if(avail)
        {
            strDest.resize(avail);
            oGzipper.Get(reinterpret_cast<unsigned char*>(&strDest[0]), strDest.size());
        }
    }
    catch(CryptoPP::InvalidDataFormat& e)
    {
        LOG4_ERROR("%s", e.GetWhat().c_str());
        return(false);
    }

    return (true);
}

bool ThunderCodec::Gunzip(const std::string& strSrc, std::string& strDest)
{
    try
    {
        CryptoPP::Gunzip oUnZipper;
        oUnZipper.Put(reinterpret_cast<const unsigned char*>(strSrc.c_str()), strSrc.size());
        oUnZipper.MessageEnd();
        CryptoPP::word64 avail = oUnZipper.MaxRetrievable();
        if(avail)
        {
            strDest.resize(avail);
            oUnZipper.Get(reinterpret_cast<unsigned char*>(&strDest[0]), strDest.size());
        }
    }
    catch(CryptoPP::InvalidDataFormat& e)
    {
        LOG4_ERROR("%s", e.GetWhat().c_str());
        return(false);
    }
    return (true);
}

bool ThunderCodec::Rc5Encrypt(const std::string& strSrc, std::string& strDest)
{
    rc5UserKey *pKey;
    rc5CBCAlg *pAlg;
    unsigned char szIv[20] = {"2017-08-10 08:53:47"};
    std::vector<char> vecKey(GetKey().size());
    std::vector<char> vecPlain(strSrc.size());
    std::vector<char> vecCipher(strSrc.size() * 2);
    char* ucKey = vecKey.data();
    char* pPlain = vecPlain.data();
    char* pCipher = vecCipher.data();
    int cipher_length = 0;
    int numBytesOut = 0;

    memcpy(ucKey, GetKey().c_str(), GetKey().size());
    memcpy(pPlain, strSrc.c_str(), strSrc.size());
    pKey = RC5_Key_Create();
    RC5_Key_Set(pKey, GetKey().size(), reinterpret_cast<unsigned char*>(ucKey));

    pAlg = RC5_CBC_Create(1, 16, RC5_FIRST_VERSION, BB, szIv);
    (void) RC5_CBC_Encrypt_Init(pAlg, pKey);
    (void) RC5_CBC_Encrypt_Update(pAlg, strSrc.size(), reinterpret_cast<unsigned char*>(pPlain),
                    &(numBytesOut), strSrc.size() * 2, reinterpret_cast<unsigned char*>(pCipher));
    cipher_length += numBytesOut;
    (void) RC5_CBC_Encrypt_Final(pAlg, &(numBytesOut),
                    strSrc.size() * 2 - cipher_length, reinterpret_cast<unsigned char*>(pCipher) + cipher_length);
    cipher_length += numBytesOut;
    strDest.assign(reinterpret_cast<const char*>(pCipher), cipher_length);
    RC5_Key_Destroy(pKey);
    RC5_CBC_Destroy(pAlg);
    return(true);
}

bool ThunderCodec::Rc5Decrypt(const std::string& strSrc, std::string& strDest)
{
    rc5UserKey *pKey;
    rc5CBCAlg *pAlg;
    unsigned char szIv[20] = {"2017-08-10 08:53:47"};
    std::vector<char> vecKey(GetKey().size());
    std::vector<char> vecCipher(strSrc.size());
    std::vector<char> vecPlain(strSrc.size());
    char* ucKey = vecKey.data();
    char* pCipher = vecCipher.data();
    char* pPlain = vecPlain.data();
    int plain_length = 0;
    int numBytesOut = 0;
    int fillBytes = 0;

    memcpy(ucKey, GetKey().c_str(), GetKey().size());
    memcpy(pCipher, strSrc.c_str(), strSrc.size());
    pKey = RC5_Key_Create();
    RC5_Key_Set(pKey, GetKey().size(), reinterpret_cast<unsigned char*>(ucKey));

    pAlg = RC5_CBC_Create(1, 16, RC5_FIRST_VERSION, BB, szIv);
    (void) RC5_CBC_Decrypt_Init(pAlg, pKey);
    (void) RC5_CBC_Decrypt_Update(pAlg, strSrc.size(), reinterpret_cast<unsigned char*>(pCipher),
                    &(numBytesOut), reinterpret_cast<unsigned char*>(pPlain));
    fillBytes = static_cast<int>(pPlain[numBytesOut - 1]);
    plain_length += numBytesOut - fillBytes;
    strDest.assign(reinterpret_cast<const char*>(pPlain), plain_length);
    RC5_Key_Destroy(pKey);
    RC5_CBC_Destroy(pAlg);
    return(true);
}

bool ThunderCodec::AesEncrypt(const std::string& strSrc, std::string& strDest)
{
    try
    {
        CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption oAes;
        oAes.SetKeyWithIV(reinterpret_cast<const unsigned char*>(GetKey().c_str()), 16,
                          reinterpret_cast<const unsigned char*>("2015-08-10 08:53:47"));
        CryptoPP::StreamTransformationFilter oEncryptor(
                        oAes, nullptr, CryptoPP::BlockPaddingSchemeDef::PKCS_PADDING);
        for (size_t i = 0; i < strSrc.size(); ++i)
        {
            oEncryptor.Put(static_cast<unsigned char>(strSrc[i]));
        }
        oEncryptor.MessageEnd();
        size_t length = oEncryptor.MaxRetrievable();
        strDest.resize(length, 0);
        oEncryptor.Get(reinterpret_cast<unsigned char*>(&strDest[0]), length);
    }
    catch(CryptoPP::InvalidDataFormat& e)
    {
        LOG4_ERROR("%s", e.GetWhat().c_str());
        return(false);
    }
    return(true);
}

bool ThunderCodec::AesDecrypt(const std::string& strSrc, std::string& strDest)
{
    try
    {
        CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption oAes;
        oAes.SetKeyWithIV(reinterpret_cast<const unsigned char*>(GetKey().c_str()), 16,
                          reinterpret_cast<const unsigned char*>("2015-08-10 08:53:47"));
        CryptoPP::StreamTransformationFilter oDecryptor(
                        oAes, nullptr, CryptoPP::BlockPaddingSchemeDef::PKCS_PADDING);
        for (size_t i = 0; i < strSrc.size(); ++i)
        {
            oDecryptor.Put(static_cast<unsigned char>(strSrc[i]));
        }
        oDecryptor.MessageEnd();
        size_t length = oDecryptor.MaxRetrievable();
        strDest.resize(length, 0);
        oDecryptor.Get(reinterpret_cast<unsigned char*>(&strDest[0]), length);
    }
    catch(CryptoPP::InvalidDataFormat& e)
    {
        LOG4_ERROR("%s", e.GetWhat().c_str());
        return(false);
    }
    return(true);
}


bool ThunderCodec::Aes256Encrypt(const std::string& strSrc, std::string& strDest,const std::string & aes_key)
{
    strDest = strSrc;//todo
    return true;
}

bool ThunderCodec::Aes256Decrypt(const std::string& strSrc, std::string& strDest,const std::string & aes_key)
{
    strDest = strSrc;//todo
    return true;
}


} /* namespace net */
