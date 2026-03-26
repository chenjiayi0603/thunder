/*******************************************************************************
 * Project:  Net
 * @file     Worker.hpp
 * @brief    工作者
 * @author   cjy
 * @date:    2017年7月27日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef Worker_HPP_
#define Worker_HPP_

#include <memory>
#include "../NetDefine.hpp"
#include "../NetError.hpp"
#include "Interface.hpp"

#include "labor/Labor.hpp"
#include "labor/WorkerContext.hpp"
#include "step/StepNode.hpp"
#include "step/MysqlStep.hpp"

namespace net
{

class Worker;

struct tagIoWatcherData
{
    int iFd = 0;
    uint32 ulSeq = 0;
    Worker* pWorker = nullptr;     // 不在结构体析构时回收
    tagIoWatcherData() = default;
};

/**
 * @brief 子进程工作者
 * @note 处理逻辑业务,Worker是子进程,Manager是父进程
 */
class Worker : public Labor, private WorkerRuntimeContext
{
public:
	Worker() = default;
    Worker(const std::string& strWorkPath, int iControlFd, int iDataFd, int iWorkerIndex, util::CJsonObject& oJsonConf);
    ~Worker();
    void Run();
    /**
	* @brief libev异步回调函数
	* @param loop libev循环对象
	* @param watcher 观察者对象（包括信号ev_signal、io ev_io、定时器ev_timer）
	*/
	static void TerminatedCallback(struct ev_loop* loop, struct ev_signal* watcher, int revents);
	static void IdleCallback(struct ev_loop* loop, struct ev_idle* watcher, int revents);
	static void IoCallback(struct ev_loop* loop, struct ev_io* watcher, int revents);
	static void IoTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);
	static void PeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);  // 周期任务回调，用于替换IdleCallback
	static void ShortPeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);
	static void StepTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);
	static void SessionTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);

	/**
	* @brief 异步回调函数具体实现
	* @param pData 观察者数据
	*/
	bool IoRead(tagIoWatcherData* pData, struct ev_io* watcher);
	bool IoWrite(tagIoWatcherData* pData, struct ev_io* watcher);
	bool IoError(tagIoWatcherData* pData, struct ev_io* watcher);
	bool IoTimeout(struct ev_timer* watcher, bool bCheckBeat = true);
	bool StepTimeout(Step* pStep, struct ev_timer* watcher);
	bool SessionTimeout(Session* pSession, struct ev_timer* watcher);
	bool FdTransfer();
	bool RecvDataAndDispose(tagIoWatcherData* pData, struct ev_io* watcher);

	/**
	* @brief redis异步回调函数
	* @param c redis连接对象
	* @param status 回调状态
	*/
	static void RedisConnectCallback(const redisAsyncContext *c, int status);
	static void RedisDisconnectCallback(const redisAsyncContext *c, int status);
	static void RedisCmdCallback(redisAsyncContext *c, void *reply, void *privdata);
	static void RedisClusterConnectCallback(const redisAsyncContext *c, int status);
	static void RedisClusterDisconnectCallback(const redisAsyncContext *c, int status);
	static void RedisClusterCmdCallback(redisClusterAsyncContext *acc, void *r, void *privdata);
	/**
	* @brief redis异步回调函数
	* @param pData 观察者数据
	*/
	bool OnRedisConnect(const redisAsyncContext *c, int status);
	bool OnRedisDisconnect(const redisAsyncContext *c, int status);
	bool OnRedisCmdResult(redisAsyncContext *c, void *reply, void *privdata);
	bool OnRedisClusterCmdResult(redisClusterAsyncContext *acc, void *r, void *privdata);
	/**
	* @brief 进程处理
	*/
	void OnTerminated(struct ev_signal* watcher);
	bool CheckParent();
	bool CheckShareMem();
	bool SendToParent(const MsgHead& oMsgHead,const MsgBody& oMsgBody);
public:
	/**
	* @brief 节点信息
	*/
	virtual void SetProcessName(const util::CJsonObject& oJsonConf)override;
    virtual const std::string& GetWorkerIdentify()override;
    virtual int GetWorkerIndex() const override{return(iWorkerIndex);}
    /**
	* @brief 注册步骤
	*/
    virtual bool RegisterCallback(Step* pStep, ev_tstamp dTimeout = 0.0)override;
    /**
   	* @brief 删除步骤
   	*/
    virtual void DeleteCallback(Step* pStep)override;
    /**
	* @brief 注册会话
	*/
    virtual bool RegisterCallback(Session* pSession)override;
    /**
	* @brief 删除会话
	*/
    virtual void DeleteCallback(Session* pSession)override;
    /**
	* @brief 注册RedisStep
	*/
    virtual bool RegisterCallback(const redisAsyncContext* pRedisContext, RedisStep* pRedisStep)override;
    /**
	* @brief 重置定时器
	*/
    virtual bool ResetTimeout(Step* pStep, struct ev_timer* watcher)override;
    /**
	* @brief 获取会话
	*/
    virtual Session* GetSession(uint64 uiSessionId, const std::string& strSessionClass = "net::Session")override;
    virtual Session* GetSession(const std::string& strSessionId, const std::string& strSessionClass = "net::Session")override;
    virtual bool ExecStep(uint32 uiCallerStepSeq, uint32 uiCalledStepSeq,int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "")override;
    virtual bool ExecStep(uint32 uiCalledStepSeq,int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "")override;
    virtual bool ExecStep(Step* pStep,ev_tstamp dTimeout = 0.0,int iErrno = 0, const std::string& strErrMsg = "", const std::string& strErrShow = "")override;
    virtual bool ExecStep(RedisStep* pStep)override;
    virtual Step* GetStep(uint32 uiStepSeq)override;
    /**
	* @brief 线程池等跨线程完成时校验 Step 是否仍由本 Worker 持有，避免晚到 resume UAF
	*/
    bool IsRegisteredStep(uint32 uiStepSeq, const Step* pStep);
    /**
	* @brief 发送数据
	* @note 含自动连接成功后，发送待发送数据
	* param loop libev循环对象
	*/
    virtual bool SendTo(const tagMsgShell& stMsgShell)override;
    virtual bool SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendTo(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody)override;
    virtual bool SendTo(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendTo(const std::string& strIdentify,uint32 cmd,uint32 seq,const std::string & strBody)override;
    virtual bool SendToSession(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendToSession(const std::string& strNodeType,const std::string& strTargetId,uint32 cmd,uint32 seq,const std::string & strBody)override;
    virtual bool SendToClientSession(const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    /**
	 * @brief 网关发送到客户端
	 * @note 网关发送到客户端
	 * @param stMsgShell 消息外壳
	 * @param cmd 指令
	 * @param seq 序号
	 * @param strBody 数据体
	 * @param status 状态
	 * @param aesEnc 是否aes加密
	 * @return 是否发送成功
	 */
    virtual bool SendToClientGate(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody,uint32 status = 0,bool aesEnc = true)override;
    virtual bool SendToClientGate(uint32 cmd,uint32 seq,const MsgBody& oMsgBody,uint32 status = 0,bool aesEnc = true)override;

    virtual bool SendToAllClientGate(uint32 cmd,const MsgBody& oMsgBody,uint32 status = 0,bool aesEnc = true)override;
    /**
	* @brief 从网关发送数据到客户端
	* @param strIdentify 连接标识符
	* @param oMsgHead 数据包头
	* @param oMsgBody 数据包体
	*/
    virtual bool SendToNext(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendToNext(const std::string& strNodeType,uint32 cmd,uint32 seq,const std::string & strBody)override;
    virtual bool SendToWithMod(const std::string& strNodeType, uint64 uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendToConHash(const std::string& strNodeType, uint64 uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendToConHash(const std::string& strNodeType, const std::string& strFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendToNodeType(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendTo(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg, Step* pStep = nullptr)override;
    virtual bool SentTo(const std::string& strHost, int iPort, const std::string& strUrlPath, const HttpMsg& oHttpMsg, Step* pStep = nullptr)override;
    virtual bool SetConnectIdentify(const tagMsgShell& stMsgShell, const std::string& strIdentify)override;
    virtual bool AutoSend(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
	virtual bool AutoSend(const std::string& strHost, int iPort, const std::string& strUrlPath, const HttpMsg& oHttpMsg, Step* pStep = nullptr)override;
    virtual bool AutoRedisCmd(const std::string& strHost, int iPort, RedisStep* pRedisStep,const std::string &strPassword = "")override;
    virtual bool AutoRedisCluster(const std::string& sAddrList, RedisStep* pRedisStep)override;
    virtual bool AutoConnect(const std::string& strIdentify)override;
    virtual bool HttpsGet(const std::string & strUrl, std::string & strResponse,const std::string& strUserpwd = "",
    		util::CurlClient::eContentType eType = util::CurlClient::eContentType_none,const std::string& strCaPath= "",int iPort = 0)override;
    virtual bool HttpsPost(const std::string & strUrl, const std::string & strFields,std::string & strResponse,const std::string& strUserpwd = "",
    		util::CurlClient::eContentType eType = util::CurlClient::eContentType_none,const std::string& strCaPath= "",int iPort = 0)override;
    /**
	* @brief 返回消息到客户端
	* @param strIdentify 连接标识符
	* @param oMsgHead 数据包头
	* @param strBody 数据包体
	*/
    virtual bool SendToClient(const tagMsgShell& stInMsgShell,const MsgHead& oInMsgHead,const std::string &strBody)override;
    virtual bool SendToClient(const std::string& strIdentify,const MsgHead& oInMsgHead,const google::protobuf::Message &message,const std::string& additional = "",const std::string& strTargetId = "",bool boJsonBody=false)override;
    virtual bool SendToClient(const tagMsgShell& stInMsgShell,const MsgHead& oInMsgHead,const google::protobuf::Message &message,const std::string& additional = "",const std::string& strTargetId = "",bool boJsonBody=false)override;
    virtual bool SendToClient(const tagMsgShell& stInMsgShell,const HttpMsg& oInHttpMsg,const std::string &strBody,int iCode=200,const std::unordered_map<std::string,std::string> &heads = std::unordered_map<std::string,std::string>())override;
    virtual bool BuildMsgBody(MsgHead& oMsgHead,MsgBody &oMsgBody,const google::protobuf::Message &message,const std::string& additional = "",const std::string& strTargetId = "",bool boJsonBody=false)override;
	virtual bool ParseMsgBody(const MsgBody& oInMsgBody,google::protobuf::Message &message)override;
	/**
	* @brief 发送消息
	* @param pUpperStep 被发送的步骤
	* @param uiCmd 指令
	* @param strBody 数据包体
	* @param callback 回调处理函数
	* @param nodeType 目标节点(nodeType支持节点类型和节点标识符)
	* @param strModFactor(发送哈希因子)
	*/
	virtual bool SendToCallback(Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StepCallback callback,const std::string &nodeTypeOrIdentify,const std::string & strModFactor="",StepParam* pStepParam=nullptr)override;
	virtual bool SendToCallback(Session* pSession,uint32 uiCmd,const std::string &strBody,SessionCallback callback,const tagMsgShell& stMsgShell,const std::string & strModFactor="",StepParam* pStepParam=nullptr)override;
    virtual bool SendToCallback(Session* pSession,const DataMem::MemOperate* pMemOper,SessionCallbackMem callback,const std::string &nodeType=REDISAGENT_NODE,const std::string & strModFactor="",StepParam* pStepParam=nullptr,uint32 uiCmd = net::CMD_REQ_STORATE)override;
    virtual bool SendToCallback(Step* pUpperStep,const DataMem::MemOperate* pMemOper,StepCallbackMem callback,const std::string &nodeType=REDISAGENT_NODE,const std::string & strModFactor="",StepParam* pStepParam=nullptr,uint32 uiCmd = net::CMD_REQ_STORATE)override;
    virtual bool SendToCallback(Session* pSession,uint32 uiCmd,const std::string &strBody,SessionCallback callback,const std::string &nodeTypeOrIdentify,const std::string & strModFactor="",StepParam* pStepParam=nullptr)override;
    virtual bool SendToCallback(Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StepCallback callback,const tagMsgShell& stMsgShell,const std::string & strModFactor="",StepParam* pStepParam=nullptr)override;
    virtual bool SendToCallback(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const DataMem::MemOperate* pMemOper,StepCallbackMem callback,const std::string &nodeType=REDISAGENT_NODE,const std::string & strModFactor="",StepParam* pStepParam=nullptr,uint32 uiCmd = net::CMD_REQ_STORATE)override;

    /**
	* @brief 断开连接
	* @param stMsgShell 被断开的连接
	* @param bMsgShellNotice 是否往上调用通知
	*/
	virtual bool Disconnect(const tagMsgShell& stMsgShell, bool bMsgShellNotice = true)override;
	virtual bool Disconnect(const std::string& strIdentify, bool bMsgShellNotice = true)override;
	virtual bool AddMsgShell(const std::string& strIdentify, const tagMsgShell& stMsgShell)override;
	virtual void DelMsgShell(const std::string& strIdentify, const tagMsgShell& stMsgShell)override;
	virtual void AddNodeIdentify(const std::string& strNodeType, const std::string& strIdentify)override;
	virtual void DelNodeIdentify(const std::string& strNodeType, const std::string& strIdentify)override;
	virtual void GetNodeIdentifys(const std::string& strNodeType, std::vector<std::string>& strIdentifys)override;
	virtual bool HasNodeIdentifys(const std::string& strNodeType)override;
	virtual bool RegisterCallback(const std::string& strIdentify, RedisStep* pRedisStep)override;
	virtual bool RegisterCallback(const std::string& strHost, int iPort, RedisStep* pRedisStep)override;
	virtual bool AddRedisContextAddr(const std::string& strHost, int iPort, redisAsyncContext* ctx)override;
	virtual void DelRedisContextAddr(const redisAsyncContext* ctx)override;
	virtual void AddInnerFd(const tagMsgShell& stMsgShell) override;
	virtual void DelInnerFd(const tagMsgShell& stMsgShell) override;
	virtual bool GetMsgShell(const std::string& strIdentify, tagMsgShell& stMsgShell)override;
	virtual bool SetClientData(const tagMsgShell& stMsgShell, util::CBuffer* pBuff)override;
	virtual bool HadClientData(const tagMsgShell& stMsgShell)override;
	virtual bool GetClientData(const tagMsgShell& stMsgShell, util::CBuffer* pBuff)override;
	virtual bool SetSessionKey(const tagMsgShell& stMsgShell,const std::string &strSessionKey)override;
	virtual std::string GetClientAddr(const tagMsgShell& stMsgShell)override;
	virtual bool SetAdditionalData(const tagMsgShell& stMsgShell,MsgBody& oMsgBody)override;
	virtual bool SetAdditionalData(const tagConnectionAttr* pConn,MsgBody& oMsgBody)override;
	virtual std::string GetConnectIdentify(const tagMsgShell& stMsgShell)override;
	virtual bool AbandonConnect(const std::string& strIdentify)override;
	virtual bool SetHeartBeat(const tagMsgShell& stMsgShell)override;
	virtual bool CheckHeartBeat(const tagMsgShell& stMsgShell)override;
	virtual const tagConnectionAttr* GetConnectionAttr(const tagMsgShell& stMsgShell)override;
	/**
	* @brief 注册MysqlStep 步骤
	* @param pMysqlStep Mysql步骤
	*/
	virtual bool RegisterCallback(MysqlStep* pMysqlStep)override;
	bool AutoMysqlCmd(MysqlStep* pMysqlStep);
	bool RegisterCallback(util::MysqlAsyncConn* pMysqlContext, MysqlStep* pMysqlStep);
	bool AddMysqlContextAddr(MysqlStep* pMysqlStep, util::MysqlAsyncConn* ctx);
	void DelMysqlContextAddr(util::MysqlAsyncConn* ctx);
	bool Dispose(util::MysqlAsyncConn *c, util::SqlTask *task, MYSQL_RES *pResultSet);
protected:
	/**
	* @brief 初始化配置
	* @param oJsonConf 配置
	*/
    virtual bool Init(util::CJsonObject& oJsonConf)override;
    bool CreateEvents();
    void AddCmd(Cmd* pCmd,int iCmd);
    void PreloadCmd();
    void Destroy();
    bool AddPeriodicTaskEvent();
    /**
	* @brief 添加事件
	* @param pConn 连接对象
	*/
    bool AddIoReadEvent(tagConnectionAttr* pConn);
    bool AddIoWriteEvent(tagConnectionAttr* pConn);
    bool RemoveIoWriteEvent(tagConnectionAttr* pConn);
    bool AddIoTimeout(int iFd, uint32 ulSeq, tagConnectionAttr* pConnAttr, ev_tstamp dTimeout = 1.0);
    /**
   	* @brief 创建连接
   	*/
    tagConnectionAttr* CreateFdAttr(int iFd, uint32 ulSeq, util::E_CODEC_TYPE eCodecType = util::CODEC_PB_INTERNAL);
    tagConnectionAttr* CreateHttpFdAttr(int iFd, uint32 ulSeq,const std::string& strHost);
    tagConnectionAttr* CreateManagerFdAttr(int iFd,uint32 ulSeq);
    tagConnectionAttr* CreateAcceptFdAttr(int iFd, uint32 ulSeq,util::E_CODEC_TYPE eCodecType);
    tagConnectionAttr* CreateConnectFdAttr(int iFd, uint32 ulSeq,const std::string & strIdentify);

    bool DestroyConnect(std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>>::iterator iter, bool bMsgShellNotice = true);
    /** Worker 退出时从 map/libev 摘除连接（含 Manager IPC），避免 Destroy() 在受保护 fd 上死循环 */
    void RemoveFdAttrForShutdown(std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>>::iterator iter);

    //消息
    void MsgShellNotice(const tagConnectionAttr* pConn);
    bool Dispose(const tagConnectionAttr* pConn,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,MsgHead& oOutMsgHead, MsgBody& oOutMsgBody);
    bool Dispose(const tagConnectionAttr* pConn,const HttpMsg& oInHttpMsg, HttpMsg& oOutHttpMsg);
public:
    //连接
    const std::unordered_map<std::string, tagMsgShell>& GetMsgShellMap()const {return mapMsgShell;}
    Nodes& GetNodesMgr() {return nodesMgr;}
    //动态库
    void LoadSo(util::CJsonObject& oSoConf,bool boForce=false);
    void ReloadSo(util::CJsonObject& oCmds);
    tagSo* LoadSoAndGetCmd(int iCmd, const std::string& strSoPath, const std::string& strSymbol, int iVersion);
    void UnloadSoAndDeleteCmd(int iCmd);
    void LoadModule(util::CJsonObject& oModuleConf,bool boForce=false);
    void ReloadModule(util::CJsonObject& oUrlPaths);
    tagModule* LoadSoAndGetModule(const std::string& strModulePath, const std::string& strSoPath, const std::string& strSymbol, int iVersion);
    void UnloadSoAndDeleteModule(const std::string& strModulePath);
private:
};

} /* namespace net */

#endif /* Worker_HPP_ */
