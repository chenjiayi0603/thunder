/*******************************************************************************
 * Project:  Net
 * @file     ManagerContext.hpp
 * @brief    Manager runtime state context
 ******************************************************************************/
#ifndef SRC_LABOR_MANAGER_CONTEXT_HPP_
#define SRC_LABOR_MANAGER_CONTEXT_HPP_

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "../NetDefine.hpp"
#include "cmd/Cmd.hpp"
#include "labor/types/ConnectionAttr.hpp"
#include "labor/types/WorkerAttr.hpp"
#include "session/Session.hpp"

namespace net
{

struct ManagerRuntimeContext
{
    int32 m_iLogLevel = log4cplus::INFO_LOG_LEVEL;
    int32 m_iWorkerBeat = 0;  ///< worker进程心跳超时时间，若大于此心跳未收到worker进程上报，则重启worker进程

    int32 m_iRefreshInterval = 0;  ///< 刷新Server的间隔周期

    std::unordered_set<std::string> m_setUpstreamTypes;  ///< 本节点关注的上游节点类型(空=全量, #38 路由按需下发)

    util::E_CODEC_TYPE m_eCodec = util::CODEC_PB_INTERNAL;  ///< 接入端编解码器
    int32 m_iS2SListenFd = -1;  ///< Server to Server监听文件描述符（Server与Server之间的连接较少，但每个Server的每个Worker均与其他Server的每个Worker相连）
    ev_timer* m_pPeriodicTaskWatcher = nullptr;  ///< 周期任务定时器

    std::unordered_map<int32, tagWorkerAttr> m_mapWorker;  ///< 业务逻辑工作进程及进程属性，key为pid
    std::unordered_map<int32, int32> m_mapWorkerRestartNum;  ///< 进程被重启次数，key为WorkerIdx
    std::unordered_map<int32, int32> m_mapWorkerFdPid;  ///< 工作进程通信FD对应的进程号
    // m_mapCenterMsgShell / m_strRaftLeaderCenterKey 已迁移至 TcpCenterConnector 插件

    std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>> m_mapFdAttr;  ///< 连接的文件描述符属性
    std::unordered_map<uint32, int32> m_mapSeq2WorkerIndex;  ///< 序列号对应的Worker进程编号（用于connect成功后，向对端Manager发送希望连接的Worker进程编号）
    std::unordered_map<in_addr_t, uint32> m_mapClientConnFrequency;  ///< 客户端连接频率 (unsigned long,uint32)
    std::unordered_map<int32, std::unique_ptr<Cmd>> m_mapSysCmd;

    std::unordered_map<std::string, std::unordered_map<std::string, Session*> > m_mapCallbackSession;

    int32 m_iConfigProcessPid = -1;
    uint32 m_iConfigProcessStartTime = 0;
    uint32 m_iConfigProcessRestartCounter = 0;

};

}  // namespace net

#endif /* SRC_LABOR_MANAGER_CONTEXT_HPP_ */
