/*******************************************************************************
 * Project:  Net
 * @file     AppMsgCodec.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年10月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <netinet/in.h>
#include "protocol/oss_sys.pb.h"
#include "AppMsgCodec.hpp"


namespace net
{

#define USE_HEAD_LEN

#pragma pack(1)

/**
 * @brief 与App通信消息头
##**客户端与服务端通信的消息结构**
**公共头描述**
公共头描述：二进制，字段名和 字节长度，统一网络字节序，16个字节。
len 4|cmd 4| seq 4| version 1| reserve 1|status 2

####**登录消息**
**1）.客户端长连接登录Request**:

二进制描述(即客户端发送的数据包的body)：

[short 2,short 2,byte 1,rsaData pb字节流, aesData pb字节流]

描述如下：

- 字段1 : 是aes加密后的数据长度，2个字节
- 字段2 : ras加密后的数据长度，2个字节
- 字段3 : ras的版本 ，1个字节
- 字段4 : RsaData被rsa加密后的byte
- 字段5 : AESData被aes加密后的byte


rsa数据对应的数据结构

message RsaData{
    int64 userId = 1; // 用户id,8个字节
    bytes ecdhPubKey = 2; // 客户端临时公钥,32字节
    string aesKey = 3; // 每次请求生成的临时aes的密钥
}

aes 数据对应的数据结构

message AESData{
    string token = 1; // 用户token
    int32 clientTime = 2; // 时间戳，秒，4个字节
    string devId = 3; // 设备id
    int64 loginSeq = 4; // 登录序列号，每次自增，8个字节
    string other=5;    // 其余数据待定
}

**2.)服务端的Response**:

服务端操作：
1. 生成LoginResponse数据，通过(客户端生成公钥与服务端的私钥生成的)密钥，将sesionKey进行加密。
2. 通过客户端给的aesKey,将LoginResponse数据加密生层byte流，返回给客户端。

message LoginRsp{
	int32 code=1;//应答业务码
	string codeMsg=2;//错误描述
	bytes ecdhServerPubKey=3;//服务端公钥，用于解密
	bytes sessionKey=4;//通过(临时客户端公钥与服务端私钥协商生成的)密钥加密的值.后续消息的加密key ；
	string sessionID=5;//会话id
	int  startSeq=6;//客户端后续消息的起始seq
	int32 serverTime = 7; // 时间戳，秒，4个字节
	string other=8;    // 其他业务数据
}
 */

const unsigned char gc_app_Rsa_ReserveBit  = 0x08;          ///< 采用rsa
const unsigned char gc_app_Aes_ReserveBit  = 0x04;          ///< 采用256位aes

//const unsigned int gc_app_Rsa_CmdBit  = 0x08000000;          ///< 采用rsa
//const unsigned int gc_app_Aes_CmdBit  = 0x04000000;          ///< 采用256位aes

struct tagAppMsgHead
{
	unsigned int len = 0;                  ///< 消息体长度（4字节）
	unsigned int cmd = 0;                     ///< 命令字/功能号（4字节）
	unsigned int seq = 0;                       ///< 序列号（4字节）
	unsigned char version = 0;                  ///< 协议版本号（1字节）
    unsigned char reserve = 0;                  ///< 保留（1字节）  黄色 01 aes 10 rsa  绿色 01 gzip压缩
    unsigned short status = 0;                ///< 校验码（2字节）
    tagAppMsgHead() = default;
};

#pragma pack()


static uint32 APP_HEAD_LEN = sizeof(tagAppMsgHead);

AppMsgCodec::AppMsgCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey)
    : ThunderCodec(eCodecType, strKey)
{
//	TestEncodeDecodeAes();
}

AppMsgCodec::~AppMsgCodec()
{
}

void AppMsgCodec::TestEncodeDecodeAes()
{
	uint64 id = util::GetUniqueId(GetLabor()->GetNodeId(), GetLabor()->GetWorkerIndex());
	std::string session_key = std::to_string(id);
	LOG4_TRACE("%s() session_key(%s)",__FUNCTION__, session_key.c_str());
	std::string srcData = "123456789";
	std::string destData;
	Aes256Encrypt(srcData,destData,session_key);
	LOG4_TRACE("%s() Aes256Encrypt srcData(%s) destData(%s)",__FUNCTION__, srcData.c_str(),destData.c_str());
	srcData.clear();
	Aes256Decrypt(destData,srcData,session_key);
	LOG4_TRACE("%s() Aes256Decrypt srcData(%s) destData(%s)",__FUNCTION__, srcData.c_str(),destData.c_str());
}

E_CODEC_STATUS AppMsgCodec::Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    tagAppMsgHead stAppMsgHead;
    stAppMsgHead.version = APP_VERSION;        // version暂时无用
    stAppMsgHead.cmd = htonl((unsigned int)(gc_uiCmdBit & oMsgHead.cmd()));
    stAppMsgHead.seq = htonl(oMsgHead.seq());
    if (gc_uiCmdReserve & oMsgHead.cmd())
    {
    	stAppMsgHead.reserve = (gc_uiCmdReserve & oMsgHead.cmd()) >> 24;//如 0x04
    }
    if (oMsgBody.has_status())
    {
    	stAppMsgHead.status = htons(oMsgBody.status());
    }
    if (oMsgBody.ByteSize() > 64000000) // pb 最大限制
    {
        LOG4_ERROR("oMsgBody.ByteSize() > 64000000");
        return(CODEC_STATUS_ERR);
    }
//    int iErrno = 0;
    int iHadWriteLen = 0;
    int iWriteLen = 0;
    int iNeedWriteLen = sizeof(stAppMsgHead);
    if (oMsgBody.body().size() == 0)    // 无包体（心跳包等）
    {
    	LOG4_TRACE("cmd %u, seq %u, body size %u", oMsgHead.cmd(), oMsgHead.seq(), oMsgBody.body().size());
#ifdef USE_HEAD_LEN
    	stAppMsgHead.len = htonl((unsigned int)APP_HEAD_LEN);
#else
    	stAppMsgHead.len = htonl((unsigned int)0);
#endif
        iWriteLen = pBuff->Write(&stAppMsgHead, iNeedWriteLen);
        LOG4_TRACE("sizeof(stAppMsgHead) = %d, iWriteLen = %d", sizeof(stAppMsgHead), iWriteLen);
        if (iWriteLen != iNeedWriteLen)
        {
            LOG4_ERROR("buff write head iWriteLen != sizeof(stAppMsgHead)");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
            return(CODEC_STATUS_ERR);
        }
        return(CODEC_STATUS_OK);
    }
    MsgBody oMsgSwitchBody;
	oMsgSwitchBody.set_body(oMsgBody.body());//只是发送body字段
	LOG4_TRACE("cmd %u, seq %u, len %u body size %u", oMsgHead.cmd(), oMsgHead.seq(), oMsgSwitchBody.ByteSize(),oMsgBody.body().size());
    iHadWriteLen += iWriteLen;
    if ((gc_app_Aes_ReserveBit & stAppMsgHead.reserve))///< 采用256位aes 0x04
	{
    	LOG4_TRACE("%s() aes Enc", __FUNCTION__);
    	const std::string& session_key = pBuff->strSessionKey;
		if (session_key.size() == 0)
		{
			LOG4_ERROR("cmd[%u] session_key.size() == 0 error", oMsgHead.cmd());
			return net::CODEC_STATUS_ERR;
		}
		//使用session_key来加密
		LOG4_TRACE("%s() session_key(%s)", __FUNCTION__,session_key.c_str());
		//加密
		std::string destData;
		if (!Aes256Encrypt(oMsgSwitchBody.SerializeAsString(),destData,session_key))
		{
			LOG4_ERROR("%s() Aes256Encrypt failed",__FUNCTION__);
			return(CODEC_STATUS_ERR);
		}
#ifdef USE_HEAD_LEN
		stAppMsgHead.len = htonl((unsigned int)destData.size() + APP_HEAD_LEN);
#else
		stAppMsgHead.len = htonl((unsigned int)destData.size());
#endif
		iWriteLen = pBuff->Write(&stAppMsgHead, iNeedWriteLen);
		LOG4_TRACE("sizeof(stAppMsgHead) = %d, iWriteLen = %d", sizeof(stAppMsgHead), iWriteLen);
		if (iWriteLen != iNeedWriteLen)
		{
			LOG4_ERROR("buff write head iWriteLen != sizeof(stAppMsgHead)");
			pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
			return(CODEC_STATUS_ERR);
		}
		iNeedWriteLen = destData.size();
		iWriteLen = pBuff->Write(destData.c_str(), destData.size());
		if (iWriteLen != iNeedWriteLen)
		{
			LOG4_ERROR("buff write head iWriteLen != sizeof(stAppMsgHead)");
			pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
			return(CODEC_STATUS_ERR);
		}
//		iHadWriteLen += iWriteLen;
	}
    else
    { // 这里不压缩也不加密
#ifdef USE_HEAD_LEN
    	stAppMsgHead.len = htonl((unsigned int)oMsgSwitchBody.ByteSize() + APP_HEAD_LEN);
#else
    	stAppMsgHead.len = htonl((unsigned int)oMsgSwitchBody.ByteSize());
#endif
    	iWriteLen = pBuff->Write(&stAppMsgHead, iNeedWriteLen);
        LOG4_TRACE("sizeof(stAppMsgHead) = %d, iWriteLen = %d", sizeof(stAppMsgHead), iWriteLen);
        if (iWriteLen != iNeedWriteLen)
        {
            LOG4_ERROR("buff write head iWriteLen != sizeof(stAppMsgHead)");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
            return(CODEC_STATUS_ERR);
        }
        iNeedWriteLen = oMsgSwitchBody.ByteSize();
        iWriteLen = pBuff->Write(oMsgSwitchBody.SerializeAsString().c_str(), oMsgSwitchBody.ByteSize());
        if (iWriteLen != iNeedWriteLen)
        {
            LOG4_ERROR("buff write head iWriteLen != sizeof(stAppMsgHead)");
            pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
            return(CODEC_STATUS_ERR);
        }
//        iHadWriteLen += iWriteLen;
    }
    LOG4_TRACE("oMsgSwitchBody.ByteSize() = %d, iWriteLen = %d(compress or encrypt maybe)", oMsgSwitchBody.ByteSize(), iWriteLen);
    return(CODEC_STATUS_OK);
}



E_CODEC_STATUS AppMsgCodec::Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    LOG4_TRACE("%s() pBuff->ReadableBytes() = %u", __FUNCTION__, pBuff->ReadableBytes());
    size_t uiHeadSize = sizeof(tagAppMsgHead);
    if (pBuff->ReadableBytes() >= uiHeadSize)
    {
    	tagAppMsgHead stAppMsgHead;
        int iReadIdx = pBuff->GetReadIndex();
        pBuff->Read(&stAppMsgHead, uiHeadSize);
        stAppMsgHead.cmd = ntohl(stAppMsgHead.cmd);
        stAppMsgHead.len = ntohl(stAppMsgHead.len);
#ifdef USE_HEAD_LEN
        if (stAppMsgHead.len >= APP_HEAD_LEN)
        {
        	stAppMsgHead.len -= APP_HEAD_LEN;
        }
        else
        {
        	LOG4_ERROR("stAppMsgHead.len(%u) < APP_HEAD_LEN (%u)", stAppMsgHead.len,APP_HEAD_LEN);
			return net::CODEC_STATUS_ERR;
        }
#endif
        stAppMsgHead.seq = ntohl(stAppMsgHead.seq);
        LOG4_TRACE("cmd %u, seq %u, len %u, pBuff->ReadableBytes() %u",stAppMsgHead.cmd, stAppMsgHead.seq, stAppMsgHead.len,pBuff->ReadableBytes());
        oMsgHead.set_cmd(stAppMsgHead.cmd);
//        oMsgHead.set_msgbody_len(stAppMsgHead.len);
        oMsgHead.set_seq(stAppMsgHead.seq);
//        oMsgHead.set_checksum(0);
        if (0 == stAppMsgHead.len)      // 心跳包无包体
        {
        	oMsgHead.set_msgbody_len(0);
            return(CODEC_STATUS_OK);
        }
        if (pBuff->ReadableBytes() >= stAppMsgHead.len)
        {
            bool bResult = false;
            if ((gc_app_Aes_ReserveBit & stAppMsgHead.reserve))///< 采用256位aes 0x04
			{
            	LOG4_TRACE("%s() aes dec", __FUNCTION__);
				const std::string& session_key = pBuff->strSessionKey;
				if (session_key.size() == 0)
				{
					LOG4_ERROR("cmd[%u] session_key.size() == 0 error", oMsgHead.cmd());
					return net::CODEC_STATUS_ERR;
				}
				//使用session_key来解密
				LOG4_TRACE("%s() session_key(%s)", __FUNCTION__,session_key.c_str());
				std::string destData;
				std::string srcData(pBuff->GetRawReadBuffer(),stAppMsgHead.len);
				if (!Aes256Decrypt(srcData,destData,session_key))
				{
					LOG4_ERROR("%s() Aes256Decrypt failed",__FUNCTION__);
					return(CODEC_STATUS_ERR);
				}
				bResult = oMsgBody.ParseFromArray(destData.c_str(), destData.size());
			}
            else
            { // 这里未压缩也未加密
                bResult = oMsgBody.ParseFromArray(pBuff->GetRawReadBuffer(), stAppMsgHead.len);
            }
            if (bResult)
            {
            	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
                pBuff->SkipBytes(stAppMsgHead.len);
                LOG4_TRACE("oMsgHead[%s], oMsgBody.ByteSize[%d]", oMsgHead.DebugString().c_str(),oMsgBody.ByteSize());
                return(CODEC_STATUS_OK);
            }
            else
            {
                LOG4_WARN("cmd[%u], seq[%lu] oMsgBody.ParseFromArray() error!", oMsgHead.cmd(), oMsgHead.seq());
                return(CODEC_STATUS_ERR);
            }
        }
        else
        {
            pBuff->SetReadIndex(iReadIdx);
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        return(CODEC_STATUS_PAUSE);
    }
}

E_CODEC_STATUS AppMsgCodec::Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    E_CODEC_STATUS status = Decode(pConn->pRecvBuff.get(),oMsgHead,oMsgBody);
    if (eConnectStatus_init == pConn->ucConnectStatus)//连接状态处理为解码一个消息成功，则连接状态完成
    {
        if (CODEC_STATUS_OK == status)
        {
            pConn->ucConnectStatus = eConnectStatus_ok;
        }
        else if (CODEC_STATUS_ERR == status)
        {
            LOG4_WARN("%s()　AppMsgCodec　need to init connect status with private pb request",__FUNCTION__);
        }
    }
    return status;
}

} /* namespace net */
