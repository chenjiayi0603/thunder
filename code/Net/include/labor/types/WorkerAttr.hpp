/*******************************************************************************
* Project:  Thunder
* @file     WorkerAttr.hpp
* @brief    Worker process attribute
******************************************************************************/
#ifndef SRC_LABOR_TYPES_WORKER_ATTR_HPP_
#define SRC_LABOR_TYPES_WORKER_ATTR_HPP_

#include <time.h>
#include "libev/ev.h"
#include "NetDefine.hpp"

struct tagWorkerAttr
{
    int iWorkerIndex = 0;                   ///< 工作进程序号
    int iControlFd = -1;                    ///< 与Manager进程通信的文件描述符（控制流）
    int iDataFd = -1;                       ///< 与Manager进程通信的文件描述符（数据流）
    int32 iLoad = 0;                        ///< 负载
    int32 iConnect = 0;                     ///< 连接数量
    int32 iRecvNum = 0;                     ///< 接收数据包数量
    int32 iRecvByte = 0;                    ///< 接收字节数
    int32 iSendNum = 0;                     ///< 发送数据包数量
    int32 iSendByte = 0;                    ///< 发送字节数
    int32 iClientNum = 0;                   ///< 客户端数量
    ev_tstamp dBeatTime = time(nullptr);    ///< 心跳时间

    tagWorkerAttr() = default;
    tagWorkerAttr(const tagWorkerAttr&) = default;
    tagWorkerAttr& operator=(const tagWorkerAttr&) = default;
};

#endif /* SRC_LABOR_TYPES_WORKER_ATTR_HPP_ */
