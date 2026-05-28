/*******************************************************************************
* Project:  Thunder
* @file     ConnectionAttr.hpp
* @brief    Connection attribute
******************************************************************************/
#ifndef SRC_LABOR_TYPES_CONNECTION_ATTR_HPP_
#define SRC_LABOR_TYPES_CONNECTION_ATTR_HPP_

#include <unordered_map>
#include <memory>
#include <cstring>
#include "libev/ev.h"
#include "NetDefine.hpp"
#include "util/CBuffer.hpp"
#include "util/StreamCodec.hpp"

namespace net { class ThunderCodec; }

enum ConnectStatus
{
    eConnectStatus_init = 0,
    eConnectStatus_connecting = 1,
    eConnectStatus_ok = 2,
};

/**
 * @brief 连接属性
 * @note  连接属性，因内部带有许多指针，并且没有必要提供深拷贝构造，所以不可以拷贝，也无需拷贝
 */
struct tagConnectionAttr
{
    static const uint32 mc_uiBeat = 0x00000001;
    static const uint32 mc_uiAlive = 0x00000007;   ///< 最近三次心跳任意一次成功则认为在线
    unsigned char ucConnectStatus = 0;
    std::shared_ptr<util::CBuffer> pRecvBuff;           ///< 后端 PendingOp 持有一份引用，防 UAF；最后引用释放时析构
    std::shared_ptr<util::CBuffer> pSendBuff;           ///< 后端 PendingOp 持有一份引用，防 UAF；最后引用释放时析构
    std::shared_ptr<util::CBuffer> pWaitForSendBuff;    ///< 等待发送的数据缓冲区
    std::shared_ptr<util::CBuffer> pClientData;         ///< 客户端相关数据
    char szRemoteAddr[32] = {0};                        ///< 对端IP地址
    util::E_CODEC_TYPE eCodecType = util::CODEC_PB_INTERNAL;  ///< 协议（编解码）类型
    ev_tstamp dActiveTime = 0;                          ///< 最后一次访问时间
    ev_tstamp dKeepAlive = 0;                           ///< 连接保持时间
    int iFd = 0;                                        ///< 文件描述符
    uint32 ulSeq = 0;                                   ///< 文件描述符创建时对应的序列号
    uint32 ulForeignSeq = 0;                            ///< 外来的seq
    uint32 ulMsgNumUnitTime = 0;                        ///< 统计单位时间内发送消息数量
    uint32 ulMsgNum = 0;                                ///< 发送消息数量
    std::string strIdentify;                            ///< 连接标识
    ev_io* pIoWatcher = nullptr;                        ///< 不在结构体析构时回收
    ev_timer* pTimeWatcher = nullptr;                   ///< 不在结构体析构时回收
    // 缓存的编解码器指针 (连接建立时填入, 避免每条请求 mapCodec.find 的 hash 查找)
    net::ThunderCodec* pCodec = nullptr;
    void* pProtoCtx = nullptr;                          ///< 协议专属上下文 (HTTP: HttpConnContext*, Arena 等)
    std::string strSessionKey;                          ///< 会话密钥

    std::unordered_map<uint32, uint32> mapCmdsUnitMsgCounter; ///< 连接的单位时间指令统计
    ev_tstamp dUnitLimitLastTime = 0;                   ///< 最后一次统计时间

    // ---- send_zc true zero-copy (THUNDER_URING_ZC_DIRECT=1) ----
    // pSendBuff/pRecvBuff/pWaitForSendBuff 为 shared_ptr，后端 PendingOp 持有
    // 一份引用。DestroyConnect 释放 mapFdAttr 中的 shared_ptr 后，若 PendingOp
    // 仍持有引用（内核 DMA 中），buffer 不会析构；NOTIF CQE 到达后 PendingOp
    // 被 delete，最后一个 shared_ptr 释放，buffer 安全析构。无需手动计数。
    //
    // 之前用 unique_ptr+zcInFlight/pendingDestroy 手动计数（v3.1），
    // 改为 shared_ptr RAII 方案：更安全（自动），且代码更简单。

    tagConnectionAttr() = default;

    bool IsVerify() const
    {
        return (pClientData != nullptr && pClientData->ReadableBytes() > 0);
    }
};

#endif /* SRC_LABOR_TYPES_CONNECTION_ATTR_HPP_ */
