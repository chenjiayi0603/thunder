/*******************************************************************************
 * Project:  Net
 * @file     Labor.hpp
 * @brief    Node工作成员
 * @author   cjy
 * @date:    2017年9月6日
 * @note     负责基础网络工作，具体工作由具有具体职责的类来负责，如Manager或Worker
 * Modify history:
 ******************************************************************************/
#ifndef SRC_NodeLabor_HPP_
#define SRC_NodeLabor_HPP_
#include <deque>
#include <functional>
#include <mutex>
#include "NetDefine.hpp"
#include "Interface.hpp"
#include "util/CommonUtils.hpp"
#include "cmd/CW.hpp"
#include "dispatcher/Nodes.hpp"
#include "labor/types/ConnectionAttr.hpp"
#include "labor/types/LoaderConfigVersionData.hpp"
#include "labor/types/RouteNoticeVersionData.hpp"
#include "labor/types/CustomConfigVersionData.hpp"
#include "storage/dataproxy.pb.h"
namespace netcustomcat {
	class CatClientConnent;
}

namespace net
{

//访问存储节点回调（Proxy\PgAgent等）
typedef void (*SessionCallbackMem)(const DataMem::MemRsp &oMemRsp,net::Session*pSession,net::StepParam * data);
typedef void (*StepCallbackMem)(const DataMem::MemRsp &oMemRsp,net::Step*pStep,net::StepParam* data);
//访问一般节点回调
typedef void (*SessionCallback)(const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,net::StepParam* data,net::Session*pSession);
typedef void (*StepCallback)(const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,net::StepParam* data,net::Step*pStep);

typedef void (*signal_callback)(struct ev_loop*,ev_signal*,int);
typedef void (*timer_callback)(struct ev_loop*,ev_timer*,int);
typedef void (*io_callback)(struct ev_loop*,ev_io*,int);

struct CoSleepAwaiter;
void CoSleepTimerTrampoline(struct ev_loop*, struct ev_timer*, int);
void PostToEventLoopAsyncCallback(struct ev_loop*, ev_async*, int);

/**
 * @brief 从节点配置解析 log_level：支持 JSON 数字或内置级别名（TRACE/DEBUG/INFO/WARN/ERROR/FATAL）。
 * @return 若存在合法 log_level 则为 true 并写入 ioLogLevel；否则 false 且 ioLogLevel 不变。
 */
bool ResolveLogLevelFromConf(const util::CJsonObject& oJsonConf, int32& ioLogLevel);

/**
 * @brief 框架层工作者抽象类
 * @note 框架层工作者抽象类，框架层工作者包括Manager和Worker
 * @note 旧版栈协程 `dispatcher/Coroutine` + `USE_COROUTINE` 已移除；业务协程请用 C++20 `Coroutine20`/`task` 体系。
 */
class Labor
{
    friend struct CoSleepAwaiter;
    friend void CoSleepTimerTrampoline(struct ev_loop*, struct ev_timer*, int);
    friend void PostToEventLoopAsyncCallback(struct ev_loop*, ev_async*, int);
public:
    Labor();
    virtual ~Labor();
    virtual bool Init(util::CJsonObject& oJsonConf){return true;}
public:     // Labor相关设置（由Cmd类或Step类调用这些方法完成Labor自身的初始化和更新，Manager和Worker类都必须实现这些方法）
    /**
	* @brief 连接成功后发送
	* @note 当前Server往另一个Server发送数据而两Server之间没有可用连接时，框架层向对端发起连接（发起连接 的过程是异步非阻塞的，
	* connect()函数返回的时候并不知道连接是否成功），并将待发送数据存放于应用层待发送缓冲区。
	* 当连接成功时将待发送数据从应用层待发送缓冲区拷贝到应用层发送缓冲区并发送。此函数由框架层自动调用，业务逻辑层无须关注。
	* @param stMsgShell 消息外壳
	 */
    virtual bool SendTo(const tagMsgShell& stMsgShell) = 0;
    /**
	 * @brief 发送数据
	 * @note 使用指定连接将数据发送。如果能直接得知stMsgShell（比如刚从该连接接收到数据，欲回确认包），就 应调用此函数发送。此函数是SendTo()函数中最高效的一个。
	 * @param stMsgShell 消息外壳
	 * @param oMsgHead 数据包头
	 * @param oMsgBody 数据包体
	 * @return 是否发送成功
	 */
    virtual bool SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody) = 0;
    /**
   	* @brief 发送
   	 * @param stMsgShell 消息外壳
   	 * @param cmd 指令
   	 * @param seq 序号
   	 * @return 是否发送成功
   	 */
    virtual bool SendTo(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody) = 0;
    /**
     * @brief 自动连接并发送
     * @note 当strIdentify对应的连接不存在时，分解strIdentify得到host、port等信息建立连接，连接成功后发 送数据。
          * 仅适用于strIdentify是合法的Server间通信标识符（IP:port:worker_index组成）。返回ture只标 识连接这个动作发起成功，不代表数据已发送成功。
     * @param strIdentify 连接标识符
     * @param oMsgHead 数据包头
     * @param oMsgBody 数据包体
     * @return 是否可以自动发送
     */
    virtual bool AutoSend(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody) = 0;
    /**
	 * @brief 设置连接的标识符信息
	 * @note 设置连接的标识符信息到框架层的连接属性tagConnectionAttr里。当连接断开时，框架层工作者可以通
	 * 过连接属性里的strIdentify调用Step::DelMsgShell(const std::string& strIdentify)删除连接标识。
	 * @param stMsgShell 消息外壳
	 * @param strIdentify 连接标识符
	 * @return 是否设置成功
	 */
	virtual bool SetConnectIdentify(const tagMsgShell& stMsgShell, const std::string& strIdentify) = 0;
    /**
	* @brief 设置进程名
	* @param oJsonConf 配置信息
	* @return 是否设置成功
	*/
   virtual void SetProcessName(const util::CJsonObject& oJsonConf) = 0;
public:     // Worker相关设置（由Cmd类或Step类调用这些方法完成数据交互，Worker类必须重新实现这些方法，Manager类可不实现）

   virtual bool AutoRedisCluster(const std::string& sAddrList, RedisStep* pRedisStep) {return false;}
   virtual bool AutoSend(const std::string& strHost, int iPort, const std::string& strUrlPath, const HttpMsg& oHttpMsg, Step* pStep = nullptr){return false;}
   /**
	* @brief 自动连接并执行redis命令
	* @param strHost redis服务所在IP
	* @param iPort redis端口
	* @param pRedisStep 执行redis命令的redis步骤实例
	* @return 是否可以执行
	*/
   virtual bool AutoRedisCmd(const std::string& strHost, int iPort, RedisStep* pRedisStep,const std::string &strPassword = "") {return(false);}
   /**
   	 * @brief 发送数据
   	 * @note 指定连接标识符将数据发送。此函数先查找与strIdentify匹配的stMsgShell，如果找到就调用
   	 * SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody)发送，
   	 * 如果未找到则调用SendToWithAutoConnect(const std::string& strIdentify,const MsgHead& oMsgHead, const MsgBody& oMsgBody)连接后再发送。
   	 * @param strIdentify 连接标识符(IP:port.worker_index, e.g 192.168.11.12:3001.1)
   	 * @param oMsgHead 数据包头
   	 * @param oMsgBody 数据包体
   	 * @return 是否发送成功
   	 */
   	virtual bool SentTo(const std::string& strHost, int iPort, const std::string& strUrlPath, const HttpMsg& oHttpMsg, Step* pStep = nullptr){return(false);}
   	/*
   	 * @brief 服务器使用的发送到客户端接口
   	 * @note 为支持对不同客户端构造不同响应消息
   	 * */
   	virtual bool ParseMsgBody(const MsgBody& oInMsgBody,google::protobuf::Message &message){return false;}
   	/**
	 * @brief 构建消息体
	 * @note 构建消息体oMsgBody
	 * @param oMsgHead 数据包头
	 * @param oMsgBody 数据包体
	 * @param message 需要填进去的pb数据
	 * @param additional 附加数据
	 * @return 是否成功
	 */
   	virtual bool BuildMsgBody(MsgHead& oMsgHead,MsgBody &oMsgBody,const google::protobuf::Message &message,const std::string& additional = "",const std::string& strTargetId = "",bool boJsonBody=false){return false;}
   	/**
	 * @brief 发送数据到客户端
	 * @note 构建消息体oMsgBody
	 * @param strIdentify 连接标识符(IP:port.worker_index, e.g 192.168.11.12:3001.1)
	 * @param oMsgHead 数据包头
	 * @param oMsgBody 数据包体
	 * @param message 需要填进去的pb数据
	 * @param additional 附加数据
	 * @return 是否成功
	 */
   	virtual bool SendToClient(const std::string& strIdentify,const MsgHead& oInMsgHead,const google::protobuf::Message &message,const std::string& additional = "",const std::string& strTargetId = "",bool boJsonBody=false){return false;}
   	virtual bool SendToClient(const tagMsgShell& stInMsgShell,const MsgHead& oInMsgHead,const google::protobuf::Message &message,const std::string& additional = "",const std::string& strTargetId = "",bool boJsonBody=false){return(false);}
   	virtual bool SendToClient(const tagMsgShell& stInMsgShell,const MsgHead& oInMsgHead,const std::string &strBody){return(false);}
   	virtual bool SendToClient(const tagMsgShell& stInMsgShell,const HttpMsg& oInHttpMsg,const std::string &strBody,int iCode=200,const std::unordered_map<std::string,std::string> &heads = std::unordered_map<std::string,std::string>()){return(false);}
   	/**
	 * @brief 发送数据到服务器（异步回调处理）
	 * @note 异步通用回调接口简化封装
	 * @param pUpperSession 上一个会话
	 * @param pUpperStep 上一个步骤
	 * @param cmd 指令
	 * @param seq 序号
	 * @param strBody 数据体
	 * @return 是否成功
	 */
   	virtual bool SendToCallback(Session* pUpperSession,const DataMem::MemOperate* pMemOper,SessionCallbackMem callback,const std::string &nodeType=REDISAGENT_NODE,const std::string & strModFactor="",StepParam* pStepParam=nullptr,uint32 uiCmd = CMD_REQ_STORATE){return false;}
   	virtual bool SendToCallback(Step* pUpperStep,const DataMem::MemOperate* pMemOper,StepCallbackMem callback,const std::string &nodeType=REDISAGENT_NODE,const std::string & strModFactor="",StepParam* pStepParam=nullptr,uint32 uiCmd = CMD_REQ_STORATE){return false;}
   	virtual bool SendToCallback(Session* pUpperSession,uint32 uiCmd,const std::string &strBody,SessionCallback callback,const std::string &nodeTypeOrIdentify,const std::string & strModFactor="",StepParam* pStepParam=nullptr){return false;}
   	virtual bool SendToCallback(Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StepCallback callback,const std::string &nodeTypeOrIdentify,const std::string & strModFactor="",StepParam* pStepParam=nullptr){return false;}
   	virtual bool SendToCallback(Session* pUpperSession,uint32 uiCmd,const std::string &strBody,SessionCallback callback,const tagMsgShell& stMsgShell,const std::string & strModFactor="",StepParam* pStepParam=nullptr){return false;}
   	virtual bool SendToCallback(Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StepCallback callback,const tagMsgShell& stMsgShell,const std::string & strModFactor="",StepParam* pStepParam=nullptr){return false;}

   	/**
	 * @brief 发送http请求数据到服务器（异步回调处理）
	 * @note 异步通用回调接口简化封装
	 * @param stMsgShell 消息壳
	 * @param oInHttpMsg http消息
	 * @return 是否成功
	 */
   	virtual bool SendToCallback(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const DataMem::MemOperate* pMemOper,StepCallbackMem callback,const std::string &nodeType=REDISAGENT_NODE,const std::string & strModFactor="",StepParam* pStepParam=nullptr,uint32 uiCmd = net::CMD_REQ_STORATE){return false;}
   	/**
   	 * @brief 发送数据
   	 * @param stMsgShell 消息外壳
   	 * @param oHttpMsg Http数据包
   	 * @param pHttpStep 步骤
   	 * @return 是否发送成功
   	 */
   	virtual bool SendTo(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg, Step* pStep = nullptr){return(false);}
	/**
	 * @brief 发送数据到服务器
	 * @note 发送数据到服务器
	 * @param strIdentify 连接标识符(IP:port.worker_index, e.g 192.168.11.12:3001.1)
	 * @param oMsgHead 数据包头
	 * @param oMsgBody 数据包体
	 * @return 是否成功
	 */
   	virtual bool SendTo(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody) {return false;}
   	/**
	 * @brief 发送数据到服务器
	 * @note 发送数据到服务器
	 * @param strIdentify 连接标识符(IP:port.worker_index, e.g 192.168.11.12:3001.1)
	 * @param cmd 指令
	 * @param seq 序号
	 * @param strBody 消息体
	 * @return 是否成功
	 */
   	virtual bool SendTo(const std::string& strIdentify,uint32 cmd,uint32 seq,const std::string & strBody) {return false;}
   	/**
	 * @brief 发送https数据，Get方法
	 * @param strUrl url
	 * @param strResponse 发送数据
	 * @param strUserpwd 用户密钥
	 * @param eType 内容类型
	 * @param strCaPath 密钥文件路径
	 * @param iPort 发送端口
	 * @return 是否发送成功
	 * @return 是否发送成功
	 */
   	virtual bool HttpsGet(const std::string & strUrl, std::string & strResponse,
   			const std::string& strUserpwd = "",util::CurlClient::eContentType eType = util::CurlClient::eContentType_none,
   			const std::string& strCaPath= "",int iPort = 0){return(false);}
   	/**
	 * @brief 发送https数据，post方法
	 * @param strUrl url
	 * @param strFields 发送数据
	 * @param strResponse 响应数据
	 * @param strUserpwd 用户密钥
	 * @param eType 内容类型
	 * @param strCaPath 密钥文件路径
	 * @param iPort 发送端口
	 * @return 是否发送成功
	 */
   	virtual bool HttpsPost(const std::string & strUrl, const std::string & strFields,std::string & strResponse,
   			const std::string& strUserpwd = "",util::CurlClient::eContentType eType = util::CurlClient::eContentType_none,
   			const std::string& strCaPath= "",int iPort = 0){return(false);}
   	/**
	 * @brief 自动连接
	 * @param strIdentify 连接描述符
	 * @return 是否成功
	 */
   	virtual bool AutoConnect(const std::string& strIdentify){return(false);}
   	/**
   	 * @brief 根据路由id自动发送到同一类型的节点
   	 * @note 根据路由id自动发送到同一类型的节点
   	 * @param strNodeType 节点类型
   	 * @param oMsgHead 数据包头
   	 * @param oMsgBody 数据包体
   	 * @return 是否发送成功
   	 */
   	virtual bool SendToSession(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody){return(false);}
   	/**
	 * @brief 根据路由id自动发送到同一类型的节点
	 * @note 根据路由id自动发送到同一类型的节点
	 * @param strNodeType 节点类型
	 * @param strTargetId 目标ID
	 * @param cmd 指令
	 * @param seq 序号
	 * @param strBody 数据体
	 * @return 是否发送成功
	 */
   	virtual bool SendToSession(const std::string& strNodeType,const std::string& strTargetId,uint32 cmd,uint32 seq,const std::string & strBody){return(false);}
   	/**
   	 * @brief 根据路由id自动发送到客户端
   	 * @note 根据路由id自动发送到同一类型的节点
   	 * @param oMsgHead 数据包头
   	 * @param oMsgBody 数据包体
   	 * @return 是否发送成功
   	 */
   	virtual bool SendToClientSession(const MsgHead& oMsgHead, const MsgBody& oMsgBody){return(false);}
   	/**
	 * @brief 网关发送到客户端
	 * @note 网关发送到客户端
	 * @param stMsgShell 消息外壳
	 * @param cmd 指令
	 * @param seq 序号
	 * @param strBody 数据体
	 * @param aesEnc 是否aes加密
	 * @return 是否发送成功
	 */
   	virtual bool SendToClientGate(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody,uint32 status = 0,bool aesEnc = true){return(false);}
   	/**
	 * @brief 网关发送到客户端
	 * @note 网关发送到客户端
	 * @param cmd 指令
	 * @param seq 序号
	 * @param oMsgBody 数据包体
	 * @param aesEnc 是否aes加密
	 * @return 是否发送成功
	 */
   	virtual bool SendToClientGate(uint32 cmd,uint32 seq,const MsgBody& oMsgBody,uint32 status = 0,bool aesEnc = true){return(false);}
   	/**
	 * @brief 网关广播到客户端
	 * @note 网关广播到客户端
	 * @param cmd 指令
	 * @param oMsgBody 数据包体
	 * @param aesEnc 是否aes加密
	 * @return 是否发送成功
	 */
   	virtual bool SendToAllClientGate(uint32 cmd,const MsgBody& oMsgBody,uint32 status = 0,bool aesEnc = true){return(false);}
   	/**
   	 * @brief 发送到下一个同一类型的节点
   	 * @note 发送到下一个同一类型的节点，适用于对同一类型节点做轮询方式发送以达到简单的负载均衡。
   	 * @param strNodeType 节点类型
   	 * @param oMsgHead 数据包头
   	 * @param oMsgBody 数据包体
   	 * @return 是否发送成功
   	 */
   	virtual bool SendToNext(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody){return(false);}
   	/**
	 * @brief 发送到下一个同一类型的节点
	 * @note 发送到下一个同一类型的节点，适用于对同一类型节点做轮询方式发送以达到简单的负载均衡。
	 * @param strNodeType 节点类型
	 * @param cmd 指令
	 * @param seq 序号
	 * @param strBody 消息体
	 * @return 是否成功
	 */
   	virtual bool SendToNext(const std::string& strNodeType,uint32 cmd,uint32 seq,const std::string & strBody){return(false);}
   	/**
   	 * @brief 以取模方式选择发送到同一类型节点
   	 * @note 以取模方式选择发送到同一类型节点，实现简单有要求的负载均衡。
   	 * @param strNodeType 节点类型
   	 * @param uiModFactor 取模因子
   	 * @param oMsgHead 数据包头
   	 * @param oMsgBody 数据包体
   	 * @return 是否发送成功
   	 */
   	virtual bool SendToWithMod(const std::string& strNodeType, uint64 uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody){return(false);}
   	/**
   	 * @brief 以一致性哈希方式选择发送到同一类型节点
   	 * @note 以取模方式选择发送到同一类型节点，实现简单有要求的负载均衡。
   	 * @param strNodeType 节点类型
   	 * @param uiModFactor 取模因子
   	 * @param oMsgHead 数据包头
   	 * @param oMsgBody 数据包体
   	 * @return 是否发送成功
   	 */
   	virtual bool SendToConHash(const std::string& strNodeType, uint64 uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody){return(false);}
   	virtual bool SendToConHash(const std::string& strNodeType, const std::string& strFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody){return(false);}
   	/**
   	 * @brief 发送到一种类型的节点
   	 * @note 发送到同一种类型除当前节点之外的所有节点。
   	 * @param strNodeType 节点类型
   	 * @param oMsgHead 数据包头
   	 * @param oMsgBody 数据包体
   	 * @return 是否发送成功
   	 */
   	virtual bool SendToNodeType(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody){return(false);}
	/**
	 * @brief 获取Worker进程编号
	 * @return Worker进程编号
	 */
	virtual int GetWorkerIndex() const{return(0);}
	/**
	 * @brief 定时器超时处理
	 * @param watcher 定时器
	 * @param bCheckBeat 是否发送心跳检查
	 * @return Worker进程编号
	 */
    virtual bool IoTimeout(struct ev_timer* watcher, bool bCheckBeat = true){return(false);}
    /**
     * @brief 注册步骤回调
     * @param pStep 步骤回调
     * @param dTimeout 超时时间
     * @return 是否注册成功
     */
    virtual bool RegisterCallback(Step* pStep, double dTimeout = 0.0){return(false);}
    /**
     * @brief 删除步骤回调
     * @param pStep 步骤回调
     */
    virtual void DeleteCallback(Step* pStep){}
    /**
     * @brief 注册会话
     * @param pSession 会话
     * @return 是否注册成功
     */
    virtual bool RegisterCallback(Session* pSession){return(false);}
    /**
     * @brief 删除会话
     * @param pSession 会话
     */
    virtual void DeleteCallback(Session* pSession){}
    /**
	 * @brief 注册mysql回调
	 * @return 是否注册成功
	 */
    virtual bool RegisterCallback(MysqlStep* pMysqlStep){return(false);}
    /**
     * @brief 注册redis回调
     * @param pRedisContext redis连接上下文
     * @param pRedisStep redis步骤
     * @return 是否注册成功
     */
    virtual bool RegisterCallback(const redisAsyncContext* pRedisContext, RedisStep* pRedisStep){return(false);}
    /**
     * @brief 延迟Step超时时间（重新设置超时时间）
     * @param pStep 被延迟的Step
     * @param watcher Step的定时观察对象
     * @return 是否设置成功
     */
    virtual bool ResetTimeout(Step* pStep, struct ev_timer* watcher){return(false);}
    /**
     * @brief 获取Session实例
     * @param uiSessionId 会话ID
     * @return 会话实例（返回NULL表示不存在uiSessionId对应的会话实例）
     */
    virtual Session* GetSession(uint64 uiSessionId, const std::string& strSessionClass = "net::Session"){return(nullptr);}
    virtual Session* GetSession(const std::string& strSessionId, const std::string& strSessionClass = "net::Session"){return(nullptr);}
    /**
	* @brief 添加内部通信连接信息
	* @param stMsgShell 消息外壳
	*/
    virtual void AddInnerFd(const tagMsgShell& stMsgShell) {}
    /**
	* @brief 添加删除通信连接信息
	* @param stMsgShell 消息外壳
	*/
    virtual void DelInnerFd(const tagMsgShell& stMsgShell) {}
    /**
     * @brief 添加指定标识的消息外壳
     * @note 添加指定标识的消息外壳由Cmd类实例和Step类实例调用，该调用会在Step类中添加一个标识
     * 和消息外壳的对应关系，同时Worker中的连接属性也会添加一个标识。
     * @param strIdentify 连接标识符(IP:port.worker_index, e.g 192.168.11.12:3001.1)
     * @param stMsgShell  消息外壳
     * @return 是否添加成功
     */
    virtual bool AddMsgShell(const std::string& strIdentify, const tagMsgShell& stMsgShell){return(false);}
    /**
     * @brief 删除指定标识的消息外壳
     * @note 删除指定标识的消息外壳由Worker类实例调用，在IoError或IoTimeout时调
     * 用。
     */
    virtual void DelMsgShell(const std::string& strIdentify, const tagMsgShell& stMsgShell){}
    /**
     * @brief 添加标识的节点类型属性
     * @note 添加标识的节点类型属性，用于以轮询方式向同一节点类型的节点发送数据，以
     * 实现简单的负载均衡。只有Server间的各连接才具有NodeType属性，客户端到Access节
     * 点的连接不具有NodeType属性。
     * @param strNodeType 节点类型
     * @param strIdentify 连接标识符
     */
    virtual void AddNodeIdentify(const std::string& strNodeType, const std::string& strIdentify){}
    /**
     * @brief 删除标识的节点类型属性
     * @note 删除标识的节点类型属性，当一个节点下线，框架层会自动调用此函数删除标识 的节点类型属性。
     * @param strNodeType 节点类型
     * @param strIdentify 连接标识符
     */
    virtual void DelNodeIdentify(const std::string& strNodeType, const std::string& strIdentify){}
    /**
     * @brief 获取标识的节点类型属性列表
     * @param strNodeType 节点类型
     * @param strIdentifys 连接标识符列表
     */
    virtual void GetNodeIdentifys(const std::string& strNodeType, std::vector<std::string>& strIdentifys){}
    /**
	 * @brief 有该标识的节点类型属性列表
	 * @param strNodeType 节点类型
	 */
	virtual bool HasNodeIdentifys(const std::string& strNodeType){return(false);}
    /**
     * @brief 注册redis回调
     * @param strIdentify redis节点标识(192.168.16.22:9988形式的IP+端口)
     * @param pRedisStep redis步骤实例
     * @return 是否注册成功
     */
    virtual bool RegisterCallback(const std::string& strIdentify, RedisStep* pRedisStep){return(false);}
    /**
     * @brief 注册redis回调
     * @param strHost redis节点IP
     * @param iPort redis端口
     * @param pRedisStep redis步骤实例
     * @return 是否注册成功
     */
    virtual bool RegisterCallback(const std::string& strHost, int iPort, RedisStep* pRedisStep){return(false);}
    /**
     * @brief 添加指定标识的redis context地址
     * @note 添加指定标识的redis context由Worker调用，该调用会在Step类中添加一个标识和redis context的对应关系。
     */
    virtual bool AddRedisContextAddr(const std::string& strHost, int iPort, redisAsyncContext* ctx){return(false);}
    /**
     * @brief 删除指定标识的redis context地址
     * @note 删除指定标识的到redis地址的对应关系（此函数被调用时，redis context的资源已被释放或将被释放） 用。
     */
    virtual void DelRedisContextAddr(const redisAsyncContext* ctx){}
    /**
     * @brief 获取连接
     * @param strIdentify 连接标识符
     * @param stMsgShell 连接通道
     * @return 连接通道
     */
    virtual bool GetMsgShell(const std::string& strIdentify, tagMsgShell& stMsgShell){return(false);}
    /**
     * @brief 客户端连接相关数据
     */
    virtual bool SetClientData(const tagMsgShell& stMsgShell, util::CBuffer* pBuff){return(false);}
    virtual bool HadClientData(const tagMsgShell& stMsgShell){return(false);}
	virtual bool GetClientData(const tagMsgShell& stMsgShell, util::CBuffer* pBuff){return(false);}
	virtual bool SetSessionKey(const tagMsgShell& stMsgShell,const std::string &strSessionKey){return(false);}
    virtual std::string GetClientAddr(const tagMsgShell& stMsgShell){return("");}
    virtual bool SetAdditionalData(const tagMsgShell& stMsgShell,MsgBody& oMsgBody){return false;}
    virtual bool SetAdditionalData(const tagConnectionAttr* pConn,MsgBody& oMsgBody){return false;}
    virtual std::string GetConnectIdentify(const tagMsgShell& stMsgShell){return("");}
    virtual const tagConnectionAttr* GetConnectionAttr(const tagMsgShell& stMsgShell){return nullptr;}
    /**
     * @brief 断开连接
     * @note 当业务层发现连接非法（如客户端登录时无法通过验证），可调用此方法断开连接
     * @param stMsgShell 消息外壳
     * @param strIdentify 连接标识符
     * @return 断开连接结果
     */
    virtual bool Disconnect(const tagMsgShell& stMsgShell, bool bMsgShellNotice = true){return(false);}
    virtual bool Disconnect(const std::string& strIdentify, bool bMsgShellNotice = true){return(false);}
    /**
     * @brief 放弃已存在的连接
     * @note 放弃已存在连接是告知框架将连接标识符与连接的MsgShell的关系解除掉，并且将连接
     * 置为未验证状态，但并不实际断开连接。断开连接由后续步骤完成，或者等待超时断开。使用场
     * 景之一：已登录的用户在另一个终端重复登录，将之前的终端踢下线，踢下线的过程是先放弃连
     * 接，等客户端对踢下线消息响应或超时再实际把连接断开。
     * @param strIdentify 已存在连接的连接标识符
     * @return 放弃结果
     */
    virtual bool AbandonConnect(const std::string& strIdentify){return(false);}
    /**
	* @brief (内部连接)设置心跳
	* @param stMsgShell 消息外壳
	*/
    virtual bool SetHeartBeat(const tagMsgShell& stMsgShell){return(false);}
    /**
	* @brief (内部连接)检查心跳
	* @param stMsgShell 消息外壳
	*/
    virtual bool CheckHeartBeat(const tagMsgShell& stMsgShell){return(false);}
    /**
     * @brief 执行下一步
     * @param uiCallerStepSeq  调用者step seq
     * @param uiCalledStepSeq  下一步对应（被调用）的seq
     * @param iErrno     错误码
     * @param strErrMsg  错误信息
     * @param strErrShow 展示给用户的错误信息
     */
    virtual bool ExecStep(uint32 uiCallerStepSeq, uint32 uiCalledStepSeq,int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = ""){return false;}
	virtual bool ExecStep(uint32 uiCalledStepSeq,int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = ""){return false;}
	virtual bool ExecStep(Step* pStep,ev_tstamp dTimeout = 0.0,int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = ""){return false;}
	virtual bool ExecStep(RedisStep* pStep){return false;}
	virtual Step* GetStep(uint32 uiStepSeq){return nullptr;}
public:// Labor相关设置（由Cmd类或Step类调用这些方法完成Labor自身的初始化和更新，Labor自己实现这些方法）
	virtual const std::string& GetWorkerIdentify(){return m_strWorkerIdentify;}
public:
	const std::string& GetNodeIdentify();
	/**
	 * @brief 获取当前时间
	 * @note 获取当前时间，比time(nullptr)速度快消耗小，不过没有time(nullptr)精准，如果对时间精度
	 * 要求不是特别高，建议调用GetNowTime()替代time(nullptr)
	 * @return 当前时间
	 */
	time_t GetNowTime() const {return((time_t)ev_now(m_loop));}
	ev_tstamp GetTimeStamp() const {return(ev_now(m_loop));}
	struct ev_loop* GetEvLoop() const { return m_loop; }

	/**
	 * @brief 将任务投递到本 Labor 的 libev 线程执行（线程安全，可从线程池线程调用）
	 * @note 内部使用 ev_async 唤醒 loop；回调内勿长时间阻塞
	 */
	void PostToEventLoop(std::function<void()> fn);

	log4cplus::Logger GetLogger(){return(m_oLogger);}
	log4cplus::Logger GetDataLogger(){return(m_oDataLogger);}
	/**
	 * @brief 用于进程内连接对象的序号分配
	 */
	uint32 GetFdSequence()
	{
		return ++m_ulFdSequence > 0 ? m_ulFdSequence:++m_ulFdSequence;
	}
	/**
	 * @brief 用于临时缓存上下文对象的标识序号(和推送消息头序号)
	 * @note 上下文对象不会长期保存于内存中，超时一定次数会被销毁（一般是十几秒内），推送的响应（一般是一分钟内）。
	 * 在时间点低位数回到原处时（比如半小时到十几分钟），之前的上下文对象则应已销毁；推送的响应也应已接收。
	 * @return 上下文对象的标识序号
	 */
	uint32 GetSequence()
	{
		return  util::GetMsgSeq_MS();//util::GetMsgSeq_S(GetNowTime());util::GetMsgSeq_MS();
	}
	uint32 GetNodeId() const{return(m_uiNodeId);}
	void SetNodeId(uint32 uiNodeId) {m_uiNodeId = uiNodeId;}
	/**
	 * @brief 生成唯一时间戳ID
	 * @return 唯一时间戳ID
	 */
	uint64 GenerateUniqueId() const {return util::GetUniqueId(GetNodeId(), GetWorkerIndex());}
	void CloseSocket(int &s){if (s >= 0){close(s);s = -1;}}
	const std::string& GetWorkPath() const {return(m_strWorkPath);}
	const std::string& GetNodeType() const {return(m_strNodeType);}
	const std::string& GetServerName() const {return(m_strServerName);}
	/**
	 * @brief 获取客户端连接心跳时间
	 * @return 客户端连接心跳时间
	 */
	int GetClientBeatTime() const{return(m_dIoTimeout);}
	/**
	 * @brief 获取自定义配置
	 * @return 自定义配置
	 */
	const util::CJsonObject& GetCustomConf() const{return(m_oCustomConf);}
	/**
	 * @brief 获取Server间的连接IP
	 * @return Server间的连接IP
	 */
	const std::string& GetHostForServer();

	const std::string& GetHostName();

	const std::string& GetHostForClient();

	/**
	 * @brief 获取Server间的连接端口
	 * @return Server间的连接端口
	 */
	int GetPortForServer() const{return(m_iPortForServer);}

	ev_tstamp GetAddrStatInterval()const {return m_dAddrStatInterval;}
	int32 GetAddrPermitNum()const {return m_iAddrPermitNum;}

	ev_tstamp GetMsgStatInterval()const {return m_dMsgStatInterval;}
	int32 GetMsgPermitNum()const {return m_iMsgPermitNum;}

	void ResetLogLevel(log4cplus::LogLevel iLogLevel);
	void SetCustomConf(const util::CJsonObject &oCustomConf){m_oCustomConf = oCustomConf;}
	uint32 GetNodeStartTime()const {return m_uiNodeStartTime;}
	uint32 GetNodeIdWokerId()const {return (uint32(GetNodeId() << 16) & 0xFFFF0000) | (uint32(GetWorkerIndex() & 0x0000FFFF));}
	uint32 GetWorkerNum()const {return m_uiWorkerNum;}

	util::IgnoreChars m_IgnoreChars;//含默认过滤字符
	void SkipNonsenseLetters(std::string& word);
	void SkipFormatLetters(std::string& word);
	void SkipCRLetters(std::string& word);


	LoaderConfigVersionData& GetLoaderConfigVersionData(){return m_pLoaderConfigVersionData;}
	RouteNoticeVersionData& GetRouteNoticeVersionData(){return m_pRouteNoticeVersionData;}
	CustomConfigVersionData& GetCustomConfigVersionData(){return m_pCustomConfigVersionData;}
	bool IsInitLogger()const {return m_bInitLogger;}
	bool IsDataInitLogger() const {return m_bDataInitLogger;}
	const std::string& GetServerConfFileName() {return m_strServerConfFileName;}			///< 服务器配置文件名称
protected:
	void DrainPostToEventLoopQueue();
	bool InitLogger(const util::CJsonObject& oJsonConf);
	bool InitDataLogger(const util::CJsonObject& oJsonConf);

	//socket
	void  SetSocketAttr(int iFd,bool boNeedKeepInterval = true);
	bool Fd2Address(int iAiFamily,int iAcceptFd,std::string &strClientAddr);
	bool HostPort2SockAddr(const std::string& strHost, int iPort,struct sockaddr &addr,int &iFd,bool boPassive=false);
	/**
	 * @brief 添加事件
	 */
	void AddEvent(int events,ev_io* io_watcher, io_callback pFunc,int iFd);
	void AddEvent(ev_signal* signal_watcher, signal_callback pFunc, int iSignum);
	void AddEvent(ev_tstamp dTimeout,ev_timer* timer_watcher, timer_callback pFunc);

	void AddSignal(int iSignum,signal_callback callback);
	void AddStep(Step* pStep,ev_tstamp dTimeout,timer_callback callback);
	void AddSession(Session* pSession,ev_tstamp dTimeout,timer_callback callback);
	/**
	 * @brief 在 ev_loop 创建后注册跨线程投递用的 ev_async（Worker/Manager/Loader 的 CreateEvents 末尾调用）
	 */
	void InitPostToEventLoop();
	/**
	 * @brief 在 ev_loop_destroy 之前停止 ev_async 并丢弃队列中未执行的任务
	 */
	void StopPostToEventLoop();
	/**
	 * @brief 延迟超时时间（重新设置超时时间）
	 */
	void RefreshEvent(int events,ev_io* io_watcher);
	void RefreshEvent(ev_tstamp dTimeout,ev_timer* timer_watcher);
	/**
	 * @brief 删除事件
	 */
	void DelEvent(ev_timer* timer_watcher);
	void DelEvent(ev_io* io_watcher);
	template<typename WATCH,typename DATA>
	void DelEvent(WATCH* watcher,DATA *pData)
	{
		DelEvent(watcher);
		delete pData; pData = nullptr;
		watcher->data = nullptr;
		delete watcher; watcher = nullptr;
	}
protected:
	bool IsAccess()const {return (m_strHostForClient.size() > 0 && m_iPortForClient > 0);}
	uint32 m_uiNodeStartTime = ::time(nullptr);//服务节点启动时间
	//配置相关
	std::string m_strConfFile;              ///< 服务器配置文件（路径）
	std::string m_strServerConfFileName;			///< 服务器配置文件名称

	util::CJsonObject m_oLastConf;          ///< 上次加载的服务器配置
	util::CJsonObject m_oCurrentConf;       ///< 当前加载的服务器配置
	util::CJsonObject m_oCustomConf;	///< 自定义配置

	int32 m_iPortForServer = 0;         ///< Server间通信监听端口，对应 m_iS2SListenFd
	int32 m_iPortForClient = 0;         ///< 对Client通信监听端口，对应 m_iC2SListenFd
	int32 m_iGatewayPort = 0;           ///< 对Client服务的真实端口
	uint32 m_iAccessVerifyTime = 20;    ///< 外部连接校验时间（单位秒）

	ev_tstamp m_dAddrStatInterval = 60.0;          ///< IP地址数据统计时间间隔
	int32  m_iAddrPermitNum = 1000000;                ///< IP地址统计时间内允许连接次数

	ev_tstamp m_dMsgStatInterval = 0.0;       ///< 客户端连接发送数据包统计时间间隔
	int32  m_iMsgPermitNum = 0;             ///< 客户端统计时间内允许发送消息数量

	ev_tstamp m_dIoTimeout = 300.0;     //< 连接超时(单位秒)
	ev_tstamp m_dStepTimeout = 0.5;     ///< 步骤超时（单位秒）
	std::string m_strHostForServer;     ///< 对其他Server服务的IP地址（用于生成当前Worker标识）
	std::string m_strHostForClient;     ///< 对Client服务的IP地址，对应 m_iC2SListenFd
	std::string m_strGateway;           ///< 对Client服务的真实IP地址（此ip转发给m_strHostForClient）

	std::string m_strHostName;

	int32 m_iClientSocketBackLog = 65000;         ///<对客户端 socket listen backlog
	int32 m_iServerSocketBackLog = 100;         ///<对服务器 socket listen backlog

	struct ev_loop* m_loop = nullptr;

	ev_async m_evPostToLoop {};
	std::mutex m_postToLoopMutex;
	std::deque<std::function<void()>> m_postToLoopQueue;
	bool m_postToLoopStarted = false;

	std::string m_strNodeType;          ///< 节点类型，如 LOGIC
	std::string m_strWorkPath;          ///< 工作路径
	std::string m_strWorkerIdentify;    ///< 进程标识 ，如192.168.11.66:27032.0
	std::string m_strNodeIdentify;		///< 节点标识，如 192.168.11.66:16068
	std::string m_strServerName;		///< 节点名称，如Access_im、Access_web_im

	int nPid = 0;
	uint32 m_uiWorkerNum = 0;                   ///< Worker子进程数量
	uint32 m_ulFdSequence = 0;
	uint32 m_uiNodeId = 0;                      ///< 节点ID（由center分配）
	char m_pErrBuff[gc_iErrBuffLen];
	bool m_bInitLogger = false;
	bool m_bDataInitLogger = false;
	log4cplus::Logger m_oLogger;
	log4cplus::Logger m_oDataLogger;

	bool m_bCatLogSystem = true;
	netcustomcat::CatClientConnent* m_pCatClientConnent = nullptr;

	LoaderConfigVersionData m_pLoaderConfigVersionData;
	RouteNoticeVersionData m_pRouteNoticeVersionData;
	CustomConfigVersionData m_pCustomConfigVersionData;
};

} /* namespace net */
net::Labor* GetLabor();
const net::Labor* GetCLabor();

#endif /* SRC_NodeLabor_HPP_ */
