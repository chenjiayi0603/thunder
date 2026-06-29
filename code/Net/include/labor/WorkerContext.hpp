/*******************************************************************************
 * Project:  Net
 * @file     WorkerContext.hpp
 * @brief    Worker runtime state and pure state operations
 ******************************************************************************/
#ifndef SRC_LABOR_WORKER_CONTEXT_HPP_
#define SRC_LABOR_WORKER_CONTEXT_HPP_

#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include "labor/Labor.hpp"
#include "cmd/Module.hpp"
#include "codec/ThunderCodec.hpp"

namespace net
{

struct tagRedisAttr;
typedef Cmd* CreateCmd();

struct tagSo
{
    void* pSoHandle = nullptr;  // 不在本对象内管理
    std::unique_ptr<Cmd> pCmd;
    int iVersion = 0;
    std::string strSoPath;
    std::string strSymbol;
    std::string strLoadTime = util::GetCurrentTime(20);
    tagSo() = default;
};

struct tagModule
{
    std::shared_ptr<void> pSoHandle;  // #128: 引用计数，同 so_path 共享
    std::unique_ptr<Module> pModule;
    int iVersion = 0;
    std::string strSoPath;
    std::string strSymbol;
    std::string strLoadTime = util::GetCurrentTime(20);
    util::CJsonObject oConf;
    tagModule() = default;
};

typedef std::unordered_map<std::string, std::pair<std::set<util::MysqlAsyncConn*>::iterator, std::set<util::MysqlAsyncConn*> > > MysqlContextMap;

struct WorkerRuntimeContext
{
    bool boAcceptTimeoutCheck = true;  ///< 接收的连接是否超时检查

    int iManagerControlFd = 0;  ///< 与Manager父进程通信fd（控制流）
    int iManagerDataFd = 0;     ///< 与Manager父进程通信fd（数据流）
    int iWorkerIndex = 0;

    int iRecvNum = 0;    ///< 接收数据包（head+body）数量
    int iRecvByte = 0;   ///< 接收字节数（已到达应用层缓冲区）
    int iSendNum = 0;    ///< 发送数据包（head+body）数量
    int iSendByte = 0;   ///< 发送字节数（已到达系统发送缓冲区）

    uint32 iInnerFdCounter = 0;  ///< 服务端之间连接的文件描述符数量

    std::unordered_map<int32, std::unique_ptr<ThunderCodec>> mapCodec;
    std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>> mapFdAttr;
    std::unordered_map<uint32, int32> mapSeq2WorkerIndex;

    std::unordered_map<int32, std::unique_ptr<Cmd>> mapSysCmd;
    std::unordered_map<int32, std::unique_ptr<tagSo>> mapSo;
    std::unordered_map<std::string, std::unique_ptr<tagModule>> mapModule;

    std::unordered_map<uint32, std::unique_ptr<Step>> mapCallbackStep;
    std::unordered_map<int32, std::list<uint32> > mapHttpAttr;
    std::unordered_map<redisAsyncContext*, tagRedisAttr*> mapRedisAttr;
    std::unordered_map<std::string, std::unordered_map<std::string, Session*> > mapCallbackSession;

    std::unordered_map<std::string, tagMsgShell> mapMsgShell;
    Nodes nodesMgr;

    std::unordered_map<std::string, const redisAsyncContext*> mapRedisContext;
    std::unordered_map<const redisAsyncContext*, std::string> mapContextIdentify;
    std::unordered_map<std::string, redisClusterAsyncContext*> mapRedisClusterContext;
    std::unordered_map<redisClusterAsyncContext*, std::string> mapRedisClusterContextIdentify;
    std::unordered_map<redisClusterAsyncContext*, tagRedisAttr*> mapRedisClusterAttr;
    MysqlContextMap mapMysqlContext;
    std::unordered_map<util::MysqlAsyncConn*, std::string> mapMysqlContextIdentify;

    bool AddMsgShell(const std::string& strIdentify, const tagMsgShell& stMsgShell)
    {
        auto shellIter = mapMsgShell.find(strIdentify);
        if (shellIter == mapMsgShell.end())
        {
            mapMsgShell.insert(std::make_pair(strIdentify, stMsgShell));
            return true;
        }
        shellIter->second = stMsgShell;
        return false;
    }

    bool DelMsgShell(const std::string& strIdentify, const tagMsgShell& stMsgShell)
    {
        auto shellIter = mapMsgShell.find(strIdentify);
        if (shellIter == mapMsgShell.end())
        {
            return false;
        }
        if (stMsgShell.iFd && stMsgShell.ulSeq)
        {
            if (stMsgShell.iFd != shellIter->second.iFd || stMsgShell.ulSeq != shellIter->second.ulSeq)
            {
                return false;
            }
        }
        mapMsgShell.erase(shellIter);
        return true;
    }

    bool GetMsgShell(const std::string& strIdentify, tagMsgShell& stMsgShell) const
    {
        auto shellIter = mapMsgShell.find(strIdentify);
        if (shellIter == mapMsgShell.end())
        {
            return false;
        }
        stMsgShell = shellIter->second;
        return true;
    }

    bool AddRedisContextAddr(const std::string& strIdentify, const redisAsyncContext* ctx)
    {
        auto ctxIter = mapRedisContext.find(strIdentify);
        if (ctxIter != mapRedisContext.end())
        {
            return false;
        }
        mapRedisContext.insert(std::make_pair(strIdentify, ctx));
        auto identifyIter = mapContextIdentify.find(ctx);
        if (identifyIter == mapContextIdentify.end())
        {
            mapContextIdentify.insert(std::make_pair(ctx, strIdentify));
        }
        else
        {
            identifyIter->second = strIdentify;
        }
        return true;
    }

    bool DelRedisContextAddr(const redisAsyncContext* ctx, std::string* pIdentify = nullptr)
    {
        auto identifyIter = mapContextIdentify.find(ctx);
        if (identifyIter == mapContextIdentify.end())
        {
            return false;
        }
        if (pIdentify != nullptr)
        {
            *pIdentify = identifyIter->second;
        }
        auto ctxIter = mapRedisContext.find(identifyIter->second);
        if (ctxIter != mapRedisContext.end())
        {
            mapRedisContext.erase(ctxIter);
        }
        mapContextIdentify.erase(identifyIter);
        return true;
    }
};

}  // namespace net

#endif /* SRC_LABOR_WORKER_CONTEXT_HPP_ */
