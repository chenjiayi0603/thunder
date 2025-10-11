/*******************************************************************************
* Project:  Net
* @file     NetDefine.hpp
* @brief 
* @author   Tommy
* @date:    2019年7月27日
* @note
* Modify history:
******************************************************************************/
#ifndef NETDEFINE_HPP_
#define NETDEFINE_HPP_
#include "NetUtil.hpp"

#ifndef NODE_BEAT
#define NODE_BEAT 1.0
#endif

#ifndef REDISAGENT_NODE
#define REDISAGENT_NODE "REDISAGENT"
#endif

#ifndef DBAGENT_NODE
#define DBAGENT_NODE "DBAGENT"
#endif

#ifndef MONGOAGENT_NODE
#define MONGOAGENT_NODE "MONGOAGENT"
#endif

#ifndef ROUTER_NODE
#define ROUTER_NODE "ROUTER"
#endif

#ifndef AGENT_R
#define AGENT_R "DBAGENT"
#endif

#ifndef AGENT_W
#define AGENT_W "DBAGENT"
#endif

//#define USE_COROUTINE

#ifndef APP_VERSION
#define APP_VERSION 1
#endif

#ifndef SAFE_DELETE
#define SAFE_DELETE(p) {if (p) {delete p;p = nullptr;}}
#endif

#ifndef SAFE_FREE
#define SAFE_FREE(p) {if (p) {free(p);p = nullptr;}}
#endif

//如果是调试版本则定义_DEBUG，否则就注释掉
#ifndef _DEBUG
#define _DEBUG
#endif

#ifndef _DEBUG
//#pragma GCC optimize(2)
//#pragma GCC optimize(3,"Ofast","inline")
#endif

#ifndef _RUN_CLOCK
#define _RUN_CLOCK
#endif

//启用埋点日志
#ifndef ENABLE_DATA_LOG
#define ENABLE_DATA_LOG
#endif

#define MUDULE_CREATE(Module) \
extern "C" { \
net::Cmd* create() \
{\
    return new Module();\
}\
}

//是否使用Nagle算法(否则注释掉)
#define ENABLE_NAGLE
//框架日志都使用本系列日志接口
#define LOG4_FATAL(...) LOG4CPLUS_FATAL_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__)
#define LOG4_ERROR(...) LOG4CPLUS_ERROR_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__)
#define LOG4_WARN(...) LOG4CPLUS_WARN_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__)
#define LOG4_INFO(...) LOG4CPLUS_INFO_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__)
#define LOG4_DEBUG(...) LOG4CPLUS_DEBUG_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__)
#ifdef _DEBUG
#define LOG4_TRACE(...) LOG4CPLUS_TRACE_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__)
#else
#define LOG4_TRACE(...)
#endif

#define SAFE_LOG4_FATAL(...) if (GetLabor()->IsInitLogger()) {LOG4CPLUS_FATAL_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__);}
#define SAFE_LOG4_ERROR(...) if (GetLabor()->IsInitLogger()) {LOG4CPLUS_ERROR_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__);}
#define SAFE_LOG4_WARN(...) if (GetLabor()->IsInitLogger()) {LOG4CPLUS_WARN_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__);}
#define SAFE_LOG4_INFO(...) if (GetLabor()->IsInitLogger()) {LOG4CPLUS_INFO_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__);}
#define SAFE_LOG4_DEBUG(...) if (GetLabor()->IsInitLogger()) {LOG4CPLUS_DEBUG_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__);}
#define SAFE_LOG4_TRACE(...) if (GetLabor()->IsInitLogger()) {LOG4CPLUS_TRACE_FMT(GetLabor()->GetLogger(), ##__VA_ARGS__);}


#define LOG_COUNT_MAX (10)

//支持调试trace输出和生产合并输出
#define LOG4_FATAL_MERGE(fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % LOG_COUNT_MAX == 0){LOG4_FATAL("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_ERROR_MERGE(fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % LOG_COUNT_MAX == 0){LOG4_ERROR("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_WARN_MERGE(fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % LOG_COUNT_MAX == 0){LOG4_WARN("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_INFO_MERGE(fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % LOG_COUNT_MAX == 0){LOG4_INFO("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_DEBUG_MERGE(fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % LOG_COUNT_MAX == 0){LOG4_DEBUG("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#ifdef _DEBUG
#define LOG4_TRACE_MERGE(fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % LOG_COUNT_MAX == 0){LOG4_TRACE("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#else
#define LOG4_TRACE_MERGE(fmt,...)
#endif

//支持调试trace输出和生产合并输出(自定义合并输出次数)
#define LOG4_FATAL_MERGECOUNT(uiCountMax,fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % uiCountMax == 0){LOG4_FATAL("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_ERROR_MERGECOUNT(uiCountMax,fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % uiCountMax == 0){LOG4_ERROR("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_WARN_MERGECOUNT(uiCountMax,fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % uiCountMax == 0){LOG4_WARN("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_INFO_MERGECOUNT(uiCountMax,fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % uiCountMax == 0){LOG4_INFO("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#define LOG4_DEBUG_MERGECOUNT(uiCountMax,fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % uiCountMax == 0){LOG4_DEBUG("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#ifdef _DEBUG
#define LOG4_TRACE_MERGECOUNT(uiCountMax,fmt,...) do {static unsigned int uiLogCount = 0;if (++uiLogCount % uiCountMax == 0){LOG4_TRACE("[MERGE(%d)]" fmt,uiLogCount, ##__VA_ARGS__);} else {LOG4_TRACE(fmt,##__VA_ARGS__);} }while(0)
#else
#define LOG4_TRACE_MERGECOUNT(uiCountMax,fmt,...)
#endif


//埋点系列日志原始接口（目前只在子进程使用）
#ifdef ENABLE_DATA_LOG
#define DATA_LOG4_FATAL(...) if (GetLabor()->IsDataInitLogger()) {LOG4CPLUS_FATAL_FMT(GetLabor()->GetDataLogger(), ##__VA_ARGS__);}
#define DATA_LOG4_ERROR(...) if (GetLabor()->IsDataInitLogger()) {LOG4CPLUS_ERROR_FMT(GetLabor()->GetDataLogger(), ##__VA_ARGS__);}
#define DATA_LOG4_WARN(...) if (GetLabor()->IsDataInitLogger()) {LOG4CPLUS_WARN_FMT(GetLabor()->GetDataLogger(), ##__VA_ARGS__);}
#define DATA_LOG4_INFO(...) if (GetLabor()->IsDataInitLogger()) {LOG4CPLUS_INFO_FMT(GetLabor()->GetDataLogger(), ##__VA_ARGS__);}
#define DATA_LOG4_DEBUG(...) if (GetLabor()->IsDataInitLogger()) {LOG4CPLUS_DEBUG_FMT(GetLabor()->GetDataLogger(), ##__VA_ARGS__);}
#define DATA_LOG4_TRACE(...) if (GetLabor()->IsDataInitLogger()) {LOG4CPLUS_TRACE_FMT(GetLabor()->GetDataLogger(), ##__VA_ARGS__);}
#else
#define DATA_LOG4_FATAL(...)
#define DATA_LOG4_ERROR(...)
#define DATA_LOG4_WARN(...)
#define DATA_LOG4_INFO(...)
#define DATA_LOG4_DEBUG(...)
#define DATA_LOG4_TRACE(...)
#endif

#define LOAD_CONFIG(conf,name,member) if (!conf.Get(name, member)) {LOG4_ERROR("config load(%s) failed",name);return false;}

#define NACOS_CONFIGFILE ("./conf/nacos-cpp-cli.properties")

namespace net
{

/** @brief 心跳间隔时间（单位:秒） */
const int gc_iBeatInterval = NODE_BEAT;

/** @brief 每次epoll_wait能处理的最大事件数  */
const int gc_iMaxEpollEvents = 100;

/** @brief 最大缓冲区大小 */
const int gc_iMaxBuffLen = 65535;

/** @brief 错误信息缓冲区大小 */
const int gc_iErrBuffLen = 256;

const uint32 gc_uiMsgHeadSize = 15;//内部消息头大小
const uint32 gc_uiAppMsgHeadSize = 16;//app消息头大小

enum E_CMD_STATUS
{
    STATUS_CMD_START            = 0,    ///< 创建命令执行者，但未开始执行
    STATUS_CMD_RUNNING          = 1,    ///< 正在执行命令
    STATUS_CMD_COMPLETED        = 2,    ///< 命令执行完毕
    STATUS_CMD_FAULT            = 3,    ///< 命令执行出错并且不必重试
};

/**
 * @brief 消息外壳
 * @note 当外部请求到达时，因Server所有操作均为异步操作，无法立刻对请求作出响应，在完成若干个
 * 异步调用之后再响应，响应时需要有请求通道信息tagMsgShell。接收请求时在原消息前面加上
 * tagMsgShell，响应消息从通过tagMsgShell里的信息原路返回给请求方。若通过tagMsgShell
 * 里的信息无法找到请求路径，则表明请求方已发生故障或已超时被清理，消息直接丢弃。
 */
struct tagMsgShell
{
    int32 iFd = 0;          ///< 请求消息来源文件描述符
    uint32 ulSeq = 0;      ///< 文件描述符创建时对应的序列号
    tagMsgShell() = default;
    tagMsgShell(int32 _iFd,uint32 _ulSeq): iFd(_iFd), ulSeq(_ulSeq) {}
    tagMsgShell(const tagMsgShell& stMsgShell) : iFd(stMsgShell.iFd), ulSeq(stMsgShell.ulSeq) {}
    tagMsgShell& operator=(const tagMsgShell& stMsgShell)
    {
        iFd = stMsgShell.iFd;
        ulSeq = stMsgShell.ulSeq;
        return(*this);
    }
};

/**
 * @brief 心跳结构
 */
struct tagBeat
{
    int32 iId = 0;
    time_t tBeatTime = 0;
    tagBeat()  = default;
    bool operator < (const tagBeat& stBeat) const
    {
        if (tBeatTime < stBeat.tBeatTime)
        {
            return(true);
        }
        else if (tBeatTime == stBeat.tBeatTime
                && iId < stBeat.iId)
        {
            return(true);
        }
        return(false);
    }
};

/**
 * @brief 序列号结构
 */
struct tagSequence
{
    int32 iId = 0;
    uint32 ulSeq = 0;
};

}

#endif /* NETDEFINE_HPP_ */
