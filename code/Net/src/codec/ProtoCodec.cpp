/*******************************************************************************
 * Project:  Net
 * @file     ProtoCodec.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年10月6日
 * @note
 * Modify history:
 ******************************************************************************/
#include "ProtoCodec.hpp"

namespace net
{

ProtoCodec::ProtoCodec(util::E_CODEC_TYPE eCodecType, const std::string& strKey)
    : ThunderCodec(eCodecType, strKey)
{
}

ProtoCodec::~ProtoCodec()
{
}

E_CODEC_STATUS ProtoCodec::Encode(const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    LOG4_TRACE("%s() pBuff->ReadableBytes()=%zu", __FUNCTION__, pBuff->ReadableBytes());
    if (oMsgBody.ByteSize() > 64000000) // pb 最大限制
    {
        LOG4_ERROR("oHttpMsg.ByteSize() > 64000000");
        return (CODEC_STATUS_ERR);
    }
    int iErrno = 0;
    int iHadWriteLen = 0;
    int iWriteLen = 0;

    int iNeedWriteLen = oMsgHead.ByteSize();
    iWriteLen = pBuff->Write(oMsgHead.SerializeAsString().c_str(), oMsgHead.ByteSize());
    if (iWriteLen != iNeedWriteLen)
    {
        LOG4_ERROR("buff write head iWriteLen != iNeedWriteLen!");
        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
        return(CODEC_STATUS_ERR);
    }
    LOG4_TRACE("buff write head iWriteLen(%d),oMsgHead(%s,%u),oMsgHead(%s,%u)",
                    iWriteLen,oMsgHead.DebugString().c_str(),oMsgHead.ByteSize(),
                    oMsgHead.DebugString().c_str(),oMsgHead.ByteSize());
    iHadWriteLen += iWriteLen;
    if (oMsgHead.msgbody_len() == 0)    // 无包体（心跳包等）
    {
//        pBuff->Compact(8192);
        return(CODEC_STATUS_OK);
    }
    iNeedWriteLen = oMsgBody.ByteSize();
    iWriteLen = pBuff->Write(oMsgBody.SerializeAsString().c_str(), oMsgBody.ByteSize());
    if (iWriteLen == iNeedWriteLen)
    {
        LOG4_TRACE("buff write msgbody iWriteLen(%d)",iWriteLen);
        LOG4_TRACE("pBuff->ReadableBytes()=%zu", pBuff->ReadableBytes());
//        pBuff->Compact(8192);
        return(CODEC_STATUS_OK);
    }
    else
    {
        LOG4_ERROR("buff write body iWriteLen != iNeedWriteLen!");
        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteLen);
        return(CODEC_STATUS_ERR);
    }
}

E_CODEC_STATUS ProtoCodec::Decode(tagConnectionAttr* pConn,MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    // 获取/创建连接级 Arena 上下文 (挂 pProtoCtx, 连接关闭时 delete)
    auto* ctx = static_cast<ProtoConnContext*>(pConn->pProtoCtx);
    if (!ctx)
    {
        ctx = new ProtoConnContext();
        pConn->pProtoCtx = ctx;
    }

    // Arena 上分配 MsgHead/MsgBody: ParseFromArray 内部的子对象分配
    // (repeated field / string / nested msg) 全部走 Arena bump pointer 而非散列 malloc
    auto* arenaHead = google::protobuf::Arena::Create<MsgHead>(&ctx->arena);
    auto* arenaBody = google::protobuf::Arena::Create<MsgBody>(&ctx->arena);

    E_CODEC_STATUS eCodecStatus = Decode(pConn->pRecvBuff.get(), *arenaHead, *arenaBody);

    if (CODEC_STATUS_OK == eCodecStatus)
    {
        // CopyFrom 拷贝到栈对象: protobuf 深拷贝, 数据所有权转移到堆
        oMsgHead.CopyFrom(*arenaHead);
        oMsgBody.CopyFrom(*arenaBody);

        // 连接状态处理
        if(eConnectStatus_ok != pConn->ucConnectStatus)// 连接尚未完成
        {
            LOG4_TRACE("oInMsgHead.cmd(%u),ucConnectStatus(%u)", oMsgHead.cmd(),pConn->ucConnectStatus);
            if (CMD_RSP_TELL_WORKER == oMsgHead.cmd())//连接完成（回应自己的节点的数据）
            {
                pConn->ucConnectStatus = eConnectStatus_ok;
            }
            else if (CMD_RSP_TELL_WORKER > oMsgHead.cmd())
            {
                pConn->ucConnectStatus = eConnectStatus_connecting;
            }
        }
    }

    ctx->arena.Reset();  // Arena 游标归零, 内存块跨请求复用
    return eCodecStatus;
}

E_CODEC_STATUS ProtoCodec::Decode(util::CBuffer* pBuff, MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    LOG4_TRACE("%s() pBuff->ReadableBytes()=%zu, pBuff->GetReadIndex()=%zu",
                    __FUNCTION__, pBuff->ReadableBytes(), pBuff->GetReadIndex());
    if (static_cast<int>(pBuff->ReadableBytes()) >= gc_uiMsgHeadSize)
    {
    	oMsgHead.Clear();
        bool bResult = oMsgHead.ParseFromArray(pBuff->GetRawReadBuffer(), gc_uiMsgHeadSize);
        if (bResult)
        {
            if (0 == oMsgHead.msgbody_len())      // 无包体（心跳包等）
            {
                pBuff->SkipBytes(oMsgHead.ByteSize());
                return(CODEC_STATUS_OK);
            }
            if (pBuff->ReadableBytes() >= gc_uiMsgHeadSize + oMsgHead.msgbody_len())
            {
            	LOG4_TRACE("oMsgHead[%s], gc_uiMsgHeadSize[%u]", oMsgHead.DebugString().c_str(),gc_uiMsgHeadSize);
                bResult = oMsgBody.ParseFromArray(pBuff->GetRawReadBuffer() + gc_uiMsgHeadSize, oMsgHead.msgbody_len());
                if (bResult)
                {
                    pBuff->SkipBytes(gc_uiMsgHeadSize + oMsgBody.ByteSize());
                    return(CODEC_STATUS_OK);
                }
                else
                {
                    LOG4_ERROR("cmd[%u], seq[%u] oMsgBody.ParseFromArray() error!", oMsgHead.cmd(), oMsgHead.seq());
                    return(CODEC_STATUS_ERR);
                }
            }
            else
            {
                return(CODEC_STATUS_PAUSE);
            }
        }
        else
        {
            oMsgHead.Clear();
            LOG4_ERROR("oMsgHead.ParseFromArray() error!");
            return(CODEC_STATUS_ERR);
        }
    }
    else
    {
        return(CODEC_STATUS_PAUSE);
    }
}

} /* namespace net */
