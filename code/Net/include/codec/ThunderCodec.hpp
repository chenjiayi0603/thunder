/*******************************************************************************
 * Project:  Net
 * @file     ThunderCodec.hpp
 * @brief    Thunder编解码器
 * @author   cjy
 * @date:    2019年10月6日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CODEC_THUNDERCODEC_HPP_
#define SRC_CODEC_THUNDERCODEC_HPP_

#include <arpa/inet.h>
#include <zlib.h>
#include <zconf.h>

#include "log4cplus/loggingmacros.h"
#include "codec/StreamCodec.hpp"
#include "util/CBuffer.hpp"
#include "protocol/msg.pb.h"
#include "labor/Labor.hpp"
#include "labor/Attribution.hpp"
#include "NetDefine.hpp"
#include "cmd/CW.hpp"

namespace net
{

//保留字段处理需要修改
const unsigned int gc_uiGzipBit = 0x10000000;          ///< 采用zip压缩
const unsigned int gc_uiZipBit  = 0x20000000;          ///< 采用gzip压缩
const unsigned int gc_uiRc5Bit  = 0x01000000;          ///< 采用12轮Rc5加密
const unsigned int gc_uiAesBit  = 0x02000000;          ///< 采用128位aes加密

const unsigned int gc_app_Rsa_CmdBit  = 0x08000000;          ///< 采用rsa
const unsigned int gc_app_Aes_CmdBit  = 0x04000000;          ///< 采用256位aes

/**
 * @brief 编解码状态
 * @note 编解码状态用于判断编解码是否成功，其中解码发生CODEC_STATUS_ERR情况时
 * 调用方需关闭对应连接；解码发生CODEC_STATUS_PAUSE时，解码函数应将缓冲区读位
 * 置重置回读开始前的位置。
 */
enum E_CODEC_STATUS
{
    CODEC_STATUS_OK         = 0,    ///< 编解码成功
    CODEC_STATUS_ERR        = 1,    ///< 编解码失败
    CODEC_STATUS_PAUSE      = 2,    ///< 编解码暂停（数据不完整，等待数据完整之后再解码）
};

/**
 * @brief 通信编码解码器抽象类
 */
class ThunderCodec: public util::CStreamCodec
{
public:
    ThunderCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey = "That's a lizard.");
    virtual ~ThunderCodec();
    /**
     * @brief 字节流编码
     * @param[in] oMsgHead  消息包头
     * @param[in] oMsgBody  消息包体
     * @param[out] pBuff  数据缓冲区
     * @return 编解码状态
     */
    virtual E_CODEC_STATUS Encode(const MsgHead& oMsgHead,const MsgBody& oMsgBody, util::CBuffer* pBuff) = 0;

    /**
     * @brief 字节流解码
     * @param[in,out] pBuff 数据缓冲区
     * @param[out] oMsgHead 消息包头
     * @param[out] oMsgBody 消息包体
     * @return 编解码状态
     */
    virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff,MsgHead& oMsgHead, MsgBody& oMsgBody) = 0;
    /**
     * @brief 连接的字节流解码,需要处理连接初始化状态
     * @param pConn 连接封装对象
     * @param[out] oMsgHead 消息包头
     * @param[out] oMsgBody 消息包体
     * @return 编解码状态
     */
    virtual E_CODEC_STATUS Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody) = 0;

    //http类型编码器编解码
    virtual E_CODEC_STATUS Encode(const HttpMsg& oHttpMsg, util::CBuffer* pBuff) {return CODEC_STATUS_ERR;}
	virtual E_CODEC_STATUS Decode(util::CBuffer* pBuff, HttpMsg& oHttpMsg) {return CODEC_STATUS_ERR;}

	void ClearKey() {m_strKey.clear();}
	void SetKey(const std::string& strKey)
	{
		m_strKey = strKey;
	}
protected:
    const std::string& GetKey() const
    {
        return(m_strKey);
    }
    /**
     * @brief Zip压缩
     */
    bool Zip(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Zip解压
	 */
    bool Unzip(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Gzip解压
	 */
    bool Gzip(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Gzip解压
	 */
    bool Gunzip(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Rc5加密
	 */
    bool Rc5Encrypt(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Rc5解密
	 */
    bool Rc5Decrypt(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Aes128位加密
	 */
    bool AesEncrypt(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Aes128位解密
	 */
    bool AesDecrypt(const std::string& strSrc, std::string& strDest);
    /**
	 * @brief Aes256位加密
	 */
    bool Aes256Encrypt(const std::string& strSrc, std::string& strDest,const std::string & aes_key);
    /**
	 * @brief Aes256位解密
	 */
	bool Aes256Decrypt(const std::string& strSrc, std::string& strDest,const std::string & aes_key);
public:
private:
    std::string m_strKey;       // 密钥
};

} /* namespace net */

#endif /* SRC_CODEC_THUNDERCODEC_HPP_ */
