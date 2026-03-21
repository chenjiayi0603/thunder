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
    Bytef* pInBuff = (Bytef*) malloc(ulInLen);
    Bytef* pOutBuff = (Bytef*) malloc(ulOutBuffLen);
    d_stream.zalloc = (alloc_func) Z_NULL;
    d_stream.zfree = (free_func) Z_NULL;
    d_stream.opaque = (voidpf) Z_NULL;
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
            free(pInBuff);
            free(pOutBuff);
            return (false);
        }
    }

    if (d_stream.avail_in != 0) /* 输出缓冲区不够 */
    {
        iErr = deflateEnd(&d_stream);
        free(pInBuff);
        free(pOutBuff);
        return (false);
    }

    iErr = deflate(&d_stream, Z_FINISH);

    iErr = deflateEnd(&d_stream);
    if (iErr != Z_OK)
    {
        free(pInBuff);
        free(pOutBuff);
        return (false);
    }

    strDest.assign((const char*) pOutBuff, d_stream.total_out);
    free(pInBuff);
    free(pOutBuff);
    return (true);
}

bool ThunderCodec::Unzip(const std::string& strSrc, std::string& strDest)
{
    int iErr = Z_OK;
    uLongf ulInLen = strSrc.size();
    uLongf ulOutBuffLen = 1024000; /* PB的Message最大支持1MB */
    uLongf ulOutlen = 0;
    z_stream d_stream = { 0 }; /* decompression stream */
    Bytef* pInBuff = (Bytef*) malloc(ulInLen);
    Bytef* pOutBuff = (Bytef*) malloc(ulOutBuffLen);
    d_stream.zalloc = (alloc_func) Z_NULL;
    d_stream.zfree = (free_func) Z_NULL;
    d_stream.opaque = (voidpf) Z_NULL;
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
            free(pInBuff);
            free(pOutBuff);
            return (false);
        }
    }

    iErr = inflateEnd(&d_stream);
    if (iErr != Z_OK)
    {
        free(pInBuff);
        free(pOutBuff);
        return (false);
    }

    strDest.assign((const char*) pOutBuff, d_stream.total_out);
    free(pInBuff);
    free(pOutBuff);
    return (true);
}

bool ThunderCodec::Gzip(const std::string& strSrc, std::string& strDest)
{
    try
	{
		CryptoPP::Gzip oGzipper;
		oGzipper.Put((unsigned char*)strSrc.c_str(), strSrc.size());
		oGzipper.MessageEnd();

		CryptoPP::word64 avail = oGzipper.MaxRetrievable();
		if(avail)
		{
			strDest.resize(avail);
			oGzipper.Get((unsigned char*)&strDest[0], strDest.size());
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
		oUnZipper.Put((unsigned char*)strSrc.c_str(), strSrc.size());
		oUnZipper.MessageEnd();
		CryptoPP::word64 avail = oUnZipper.MaxRetrievable();
		if(avail)
		{
			strDest.resize(avail);
			oUnZipper.Get((unsigned char*)&strDest[0], strDest.size());
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
    char* ucKey = new char[GetKey().size()];
    char* pPlain = new char[strSrc.size()];
    char* pCipher = new char[strSrc.size() * 2];
    int cipher_length = 0;
    int numBytesOut = 0;

    memcpy(ucKey, GetKey().c_str(), GetKey().size());
    memcpy(pPlain, strSrc.c_str(), strSrc.size());
    pKey = RC5_Key_Create();
    RC5_Key_Set(pKey, GetKey().size(), (unsigned char*)ucKey);

    pAlg = RC5_CBC_Create(1, 16, RC5_FIRST_VERSION, BB, szIv);
    (void) RC5_CBC_Encrypt_Init(pAlg, pKey);
    (void) RC5_CBC_Encrypt_Update(pAlg, strSrc.size(), (unsigned char*)pPlain,
                    &(numBytesOut), strSrc.size() * 2, (unsigned char*)pCipher);
    cipher_length += numBytesOut;
    (void) RC5_CBC_Encrypt_Final(pAlg, &(numBytesOut),
                    strSrc.size() * 2 - cipher_length, (unsigned char*)pCipher + cipher_length);
    cipher_length += numBytesOut;
    strDest.assign((const char*)pCipher, cipher_length);
    delete[] ucKey;
    delete[] pPlain;
    delete[] pCipher;
    RC5_Key_Destroy(pKey);
    RC5_CBC_Destroy(pAlg);
    return(true);
}

bool ThunderCodec::Rc5Decrypt(const std::string& strSrc, std::string& strDest)
{
    rc5UserKey *pKey;
    rc5CBCAlg *pAlg;
    unsigned char szIv[20] = {"2017-08-10 08:53:47"};
    char* ucKey = new char[GetKey().size()];
    char* pCipher = new char[strSrc.size()];
    char* pPlain = new char[strSrc.size()];
    int plain_length = 0;
    int numBytesOut = 0;
    int fillBytes = 0;

    memcpy(ucKey, GetKey().c_str(), GetKey().size());
    memcpy(pCipher, strSrc.c_str(), strSrc.size());
    pKey = RC5_Key_Create();
    RC5_Key_Set(pKey, GetKey().size(), (unsigned char*)ucKey);

    pAlg = RC5_CBC_Create(1, 16, RC5_FIRST_VERSION, BB, szIv);
    (void) RC5_CBC_Decrypt_Init(pAlg, pKey);
    (void) RC5_CBC_Decrypt_Update(pAlg, strSrc.size(), (unsigned char*)pCipher,
                    &(numBytesOut), (unsigned char*)pPlain);
    fillBytes = (int)pPlain[numBytesOut - 1];
    plain_length += numBytesOut - fillBytes;
    strDest.assign((const char*)pPlain, plain_length);
    delete[] ucKey;
    delete[] pCipher;
    delete[] pPlain;
    RC5_Key_Destroy(pKey);
    RC5_CBC_Destroy(pAlg);
    return(true);
}

bool ThunderCodec::AesEncrypt(const std::string& strSrc, std::string& strDest)
{
	try
	{
		CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption oAes;
		oAes.SetKeyWithIV((const unsigned char*)GetKey().c_str(), 16, (const unsigned char*)"2015-08-10 08:53:47");
		CryptoPP::StreamTransformationFilter oEncryptor(
						oAes, NULL, CryptoPP::BlockPaddingSchemeDef::PKCS_PADDING);
		for (size_t i = 0; i < strSrc.size(); ++i)
		{
			oEncryptor.Put((unsigned char)strSrc[i]);
		}
		oEncryptor.MessageEnd();
		size_t length = oEncryptor.MaxRetrievable();
		strDest.resize(length, 0);
		oEncryptor.Get((unsigned char*)&strDest[0], length);
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
		oAes.SetKeyWithIV((const unsigned char*)GetKey().c_str(), 16, (const unsigned char*)"2015-08-10 08:53:47");
		CryptoPP::StreamTransformationFilter oDecryptor(
						oAes, NULL, CryptoPP::BlockPaddingSchemeDef::PKCS_PADDING);
		for (size_t i = 0; i < strSrc.size(); ++i)
		{
			// oDecryptor.Put((CryptoPP::byte)strSrc[i]);

            oDecryptor.Put((unsigned char)strSrc[i]);
		}
		oDecryptor.MessageEnd();
	size_t length = oDecryptor.MaxRetrievable();
	strDest.resize(length, 0);
	oDecryptor.Get((unsigned char*)&strDest[0], length);
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
	// strDest.resize(16 * (strSrc.size() / 16 + 1));
	// int encrypted_len = aes_encrypt( (unsigned char*) strSrc.c_str(), strSrc.size(),
	// 		 (unsigned char*) aes_key.c_str(), (unsigned char*) aes_key.substr(0, 16).c_str(),  (unsigned char*) strDest.c_str());
	// if (encrypted_len < 0)
	// {
	// 	LOG4_ERROR("aes aes_encrypt encrypted_len == %d",encrypted_len);
	// 	return false;
	// }
	// strDest.resize(encrypted_len);
	return true;
}

bool ThunderCodec::Aes256Decrypt(const std::string& strSrc, std::string& strDest,const std::string & aes_key)
{
    strDest = strSrc;//todo
	// strDest.resize(strSrc.size(),0);
    // LOG4_TRACE("aes original text : %s, size :%d", base64Encode((const char*)strSrc.c_str(), strSrc.size()), strSrc.size());
	// int decrypted_length = aes_decrypt( (unsigned char*)  strSrc.c_str(), strSrc.size(),
	// 		 (unsigned char*) aes_key.c_str(),  (unsigned char*) aes_key.substr(0, 16).c_str(),  (unsigned char*) strDest.c_str()); //对称解密
	// LOG4_TRACE("aes decrypted length =%d,encypt:%s", decrypted_length, base64Encode((const char*)strDest.c_str(), decrypted_length));
	// if (decrypted_length < 0)
	// {
	// 	LOG4_ERROR("aes decrypted decrypted_length == %d",decrypted_length);
	// 	return false;
	// }
	// strDest.resize(decrypted_length);
	return true;
}


} /* namespace net */
