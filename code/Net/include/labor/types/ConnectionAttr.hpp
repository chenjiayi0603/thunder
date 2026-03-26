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
    std::unique_ptr<util::CBuffer> pRecvBuff;           ///< 在结构体析构时回收
    std::unique_ptr<util::CBuffer> pSendBuff;           ///< 在结构体析构时回收
    std::unique_ptr<util::CBuffer> pWaitForSendBuff;    ///< 等待发送的数据缓冲区
    std::unique_ptr<util::CBuffer> pClientData;         ///< 客户端相关数据
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
    std::string strSessionKey;                          ///< 会话密钥

    std::unordered_map<uint32, uint32> mapCmdsUnitMsgCounter; ///< 连接的单位时间指令统计
    ev_tstamp dUnitLimitLastTime = 0;                   ///< 最后一次统计时间

    tagConnectionAttr() = default;

    bool IsVerify() const
    {
        return (pClientData != nullptr && pClientData->ReadableBytes() > 0);
    }
};

#endif /* SRC_LABOR_TYPES_CONNECTION_ATTR_HPP_ */
