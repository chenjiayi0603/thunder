
/*******************************************************************************
 * Project:  Net
 * @file     Worker.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年7月27日
 * @note
 * Modify history:
 ******************************************************************************/
#include <memory>
#include <algorithm>
#include <cctype>
#include "hiredis_vip/async.h"
#include "storage/RedisClusterLibevAttach.hpp"
#include "absl/status/status.h"
#include "google/protobuf/util/json_util.h"
#include "protocol/oss_sys.pb.h"
#include "../NetDefine.hpp"
#include "../NetError.hpp"
#include "labor/Labor.hpp"
#include "labor/Worker.hpp"
#include "labor/WorkerThreadPool.hpp"
#include "thread/threadpool.h"
#include <unordered_map>
#include <unordered_set>

#include "cmd/Cmd.hpp"
#include "cmd/Module.hpp"
#include "codec/ProtoCodec.hpp"
#include "codec/ClientMsgCodec.hpp"
#include "codec/HttpCodec.hpp"
#include "codec/HttpsCodec.hpp"
#include "codec/CodecWebSocketJson.hpp"
#include "codec/CodecWebSocketPb.hpp"
#include "codec/CodecWebSocketPbApp.hpp"
#include "codec/CodecCustom.hpp"
#include "codec/AppMsgCodec.hpp"
#include "step/Step.hpp"
#include "step/RedisStep.hpp"
#include "step/StepAuthRedis.hpp"
#include "step/HttpStep.hpp"
#include "session/Session.hpp"
#include "cmd/sys_cmd/CmdConnectWorker.hpp"
#include "cmd/sys_cmd/CmdToldWorker.hpp"
#include "cmd/sys_cmd/CmdUpdateNodeId.hpp"
#include "cmd/sys_cmd/CmdNodeNotice.hpp"
#include "cmd/sys_cmd/CmdBeat.hpp"
#include "cmd/sys_cmd/CmdHeartBeatRes.hpp"
#include "cmd/sys_cmd/CmdUpdateConfig.hpp"
#include "cmd/sys_cmd/CmdSetLogLevel.hpp"
#include "cmd/sys_cmd/CmdLogicConfig.hpp"
#include "cmd/sys_cmd/CmdReloadSo.hpp"
#include "cmd/sys_cmd/CmdReloadModule.hpp"
#include "cmd/sys_cmd/CmdReloadCustom.hpp"
#include "storage/RedisOperator.hpp"

namespace net
{

void Worker::TerminatedCallback(struct ev_loop* loop, struct ev_signal* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Worker* pWorker = (Worker*)watcher->data;
        pWorker->OnTerminated(watcher);  // timeout，worker进程无响应或与Manager通信通道异常，被manager进程终止时返回
    }
}

void Worker::IdleCallback(struct ev_loop* loop, struct ev_idle* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Worker* pWorker = (Worker*)watcher->data;
        pWorker->CheckParent();
    }
}

void Worker::IoCallback(struct ev_loop* loop, struct ev_io* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        tagIoWatcherData* pData = (tagIoWatcherData*)watcher->data;
        Worker* pWorker = (Worker*)pData->pWorker;
        int iFd = pData->iFd;
        uint32 ulSeq = pData->ulSeq;
        if (revents & EV_READ)
        {
            pWorker->IoRead(pData, watcher);
            // IoRead may have called DestroyConnect which frees pData; re-validate before use
            auto iter = pWorker->mapFdAttr.find(iFd);
            if (iter == pWorker->mapFdAttr.end() || iter->second->ulSeq != ulSeq)
            {
                return;
            }
        }
        if (revents & EV_WRITE)
        {
            pWorker->IoWrite(pData, watcher);
            // IoWrite may have called DestroyConnect (e.g. send error); re-validate before IoError
            auto iterW = pWorker->mapFdAttr.find(iFd);
            if (iterW == pWorker->mapFdAttr.end() || iterW->second->ulSeq != ulSeq)
            {
                return;
            }
        }
        if (revents & EV_ERROR)
        {
            pWorker->IoError(pData, watcher);
        }
    }
}

void Worker::IoTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        tagIoWatcherData* pData = (tagIoWatcherData*)watcher->data;
        Worker* pWorker = pData->pWorker;
        pWorker->IoTimeout(watcher);
    }
}

void Worker::PeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Worker* pWorker = (Worker*)(watcher->data);
        pWorker->CheckParent();
        pWorker->RefreshEvent(NODE_BEAT,watcher);
    }
}

void Worker::ShortPeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Worker* pWorker = (Worker*)(watcher->data);
        pWorker->CheckShareMem();
        pWorker->RefreshEvent(1.0,watcher);
    }
}

void Worker::StepTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Step* pStep = (Step*)watcher->data;
        ((Worker*)GetLabor())->StepTimeout(pStep, watcher);
    }
}

void Worker::SessionTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Session* pSession = (Session*)watcher->data;
        ((Worker*)GetLabor())->SessionTimeout(pSession, watcher);
    }
}

void Worker::RedisConnectCallback(const redisAsyncContext *c, int status)
{
    if (c->data != nullptr)
    {
        Worker* pWorker = (Worker*)c->data;
        pWorker->OnRedisConnect(c, status);
    }
}

void Worker::RedisDisconnectCallback(const redisAsyncContext *c, int status)
{
    if (c->data != nullptr)
    {
        Worker* pWorker = (Worker*)c->data;
        pWorker->OnRedisDisconnect(c, status);
    }
}

void Worker::RedisCmdCallback(redisAsyncContext *c, void *reply, void *privdata)
{
    if (c->data != nullptr)
    {
        Worker* pWorker = (Worker*)c->data;
        pWorker->OnRedisCmdResult(c, reply, privdata);
    }
}

void Worker::RedisClusterConnectCallback(const redisAsyncContext *c, int status)
{
    if (c->data != nullptr)
    {
        Worker* pWorker = (Worker*)c->data;
        LOG4_INFO("%s",__FUNCTION__);
    }
}

void Worker::RedisClusterDisconnectCallback(const redisAsyncContext *c, int status)
{
    if (c->data != nullptr)
    {
        Worker* pWorker = (Worker*)c->data;//目前当做集群是高可用的
        LOG4_INFO("%s",__FUNCTION__);
    }
}

void Worker::RedisClusterCmdCallback(redisClusterAsyncContext *acc, void *reply, void *privdata)
{
    if (privdata != nullptr)
    {
        Worker* pWorker = (Worker*)privdata;
        pWorker->OnRedisClusterCmdResult(acc, reply, privdata);
    }
}

Worker::Worker(const std::string& strWorkPath, int iControlFd, int iDataFd, int iWorkerIndex, util::CJsonObject& oJsonConf)
{
	m_strWorkPath = strWorkPath;
    iManagerControlFd = iControlFd;
    iManagerDataFd = iDataFd;
    iWorkerIndex = iWorkerIndex;
    if (!Init(oJsonConf))
    {
        exit(-1);
    }
    if (!CreateEvents())
    {
        exit(-2);
    }

    PreloadCmd();
    LoadSo(oJsonConf["so"]);
    LoadModule(oJsonConf["module"]);
}

Worker::~Worker()
{
    Destroy();
}

void Worker::Run()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ev_run (m_loop, 0);
}

void Worker::OnTerminated(struct ev_signal* watcher)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    int iSignum = watcher->signum;
    delete watcher;
    Destroy();
    LOG4_FATAL("terminated by signal %d!", iSignum);
    exit(iSignum);
}

bool Worker::CheckParent()// 向父进程上报当前进程负载
{
    //LOG4_TRACE("%s()", __FUNCTION__);
    pid_t iParentPid = getppid();
    if (iParentPid == 1)    // manager进程已不存在
    {
        LOG4_INFO("no manager duty exist, worker %d exit.", iWorkerIndex);
        //Destroy();
        exit(0);
    }
    MsgHead oMsgHead;
    MsgBody oMsgBody;
    util::CJsonObject oJsonLoad;
    oJsonLoad.Add("load", int32(mapFdAttr.size() + mapCallbackStep.size()));
    oJsonLoad.Add("connect", int32(mapFdAttr.size()));
    oJsonLoad.Add("recv_num", iRecvNum);
    oJsonLoad.Add("recv_byte", iRecvByte);
    oJsonLoad.Add("send_num", iSendNum);
    oJsonLoad.Add("send_byte", iSendByte);
    oJsonLoad.Add("client", int32(mapFdAttr.size() - iInnerFdCounter));
    LOG4_TRACE("%s", oJsonLoad.ToString().c_str());
    oMsgBody.set_body(oJsonLoad.ToString());
    oMsgHead.set_cmd(CMD_REQ_UPDATE_WORKER_LOAD);
    oMsgHead.set_seq(GetSequence());
    oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
    iRecvNum = iRecvByte = iSendNum = iSendByte = 0;
    SendToParent(oMsgHead,oMsgBody);
    return(true);
}

bool Worker::CheckShareMem()
{
	{// shm.seq_config（共享配置修订）
		if (GetLoaderConfigVersionData().IsConfigVersionChange())
		{
			GetLoaderConfigVersionData().UpdateLoaderConfigVersion();
			m_oLastConf = m_oCurrentConf;
			std::string configContent;
			bool boParseConf(false);
			if (GetLoaderConfigVersionData().GetServerConfigFile(configContent))
			{
				if (m_oCurrentConf.Parse(configContent))
				{
					LOG4_INFO("%s Parse config from mem ok.", __FUNCTION__);
					boParseConf = true;
				}
				else
				{
					LOG4_ERROR("%s Parse config failed.", __FUNCTION__);
				}
			}
			if (boParseConf && m_oLastConf.ToString() != m_oCurrentConf.ToString())
			{
				{//custom
					m_oCustomConf = m_oCurrentConf["custom"];
					LOG4_INFO("update custom config:%s",m_oCustomConf.ToString().c_str());
				}
				{//log_level
					if (m_oLastConf("log_level") != m_oCurrentConf("log_level"))
					{
						LOG4_INFO("update log_level:(%s)",m_oCurrentConf("log_level").c_str());
						int iLogLevel = log4cplus::INFO_LOG_LEVEL;
						(void)ResolveLogLevelFromConf(m_oCurrentConf, iLogLevel);
						ResetLogLevel(iLogLevel);
					}
				}
				{//so module // 更新动态库配置或重新加载动态库
					if (m_oLastConf["so"].ToString() != m_oCurrentConf["so"].ToString())
					{
						std::string strSo = std::move(m_oCurrentConf["so"].ToString());
						LOG4_INFO("update So:(%s)",strSo.c_str());
						util::CJsonObject oSoConfJson;
						if(!oSoConfJson.Parse(strSo))
						{
							LOG4_WARN("failed to parse oSoConfJson:(%s)",strSo.c_str());
						}
						else
						{
							LOG4_INFO("update so conf to oSoConfJson(%s) %s",
									oSoConfJson.ToString().c_str(),"force operation");
							LoadSo(oSoConfJson,true);
						}
					}
					if (m_oLastConf["module"].ToString() != m_oCurrentConf["module"].ToString())
					{
						std::string strModule = std::move(m_oCurrentConf["module"].ToString());
						LOG4_INFO("update Module:(%s)",strModule.c_str());
						util::CJsonObject oModuleConfJson;
						if(!oModuleConfJson.Parse(strModule))
						{
							LOG4_WARN("failed to parse oModuleConfJson:(%s)",strModule.c_str());
						}
						else
						{
							LOG4_INFO("update module conf to oModuleConfJson(%s) %s",
									oModuleConfJson.ToString().c_str(),"force operation");
							LoadModule(oModuleConfJson,true);
						}
					}
				}
			}
		}
	}
	{//NoticeVersion
//		LOG4_TRACE("%s() NodeNoticeVersion:%llu", __FUNCTION__,GetRouteNoticeVersionData().GetNodeNoticeVersion());
		if (GetRouteNoticeVersionData().IsNodeNoticeVersionChange())
		{
			LOG4_INFO("%s() NodeNoticeVersion:%llu bytes:%u", __FUNCTION__,
                     GetRouteNoticeVersionData().GetNodeNoticeVersion(),
                     GetRouteNoticeVersionData().GetNodeNoticeLength());
			NodeNotice oNodeNotice;
			if (GetRouteNoticeVersionData().GetNodeNotice(oNodeNotice))
			{
				GetRouteNoticeVersionData().UpdateNodeNoticeVersion();
				LOG4_INFO("%s() oNodeNotice[%s]",__FUNCTION__,oNodeNotice.DebugString().c_str());
				// Route mirror is a full snapshot for "online nodes".
				// Apply it by rebuilding the expected identify set and pruning any local id not present.
				if (oNodeNotice.node_arry_reg_size() > 0)
				{
					std::unordered_map<std::string, std::unordered_set<std::string>> expected;
					char strIdentify[64] = {0};
					for (int i = 0; i < oNodeNotice.node_arry_reg_size(); ++i)
					{
						const auto& oNodeReg = oNodeNotice.node_arry_reg(i);
						for (int j = 0; j < oNodeReg.worker_num(); ++j)
						{
							snprintf(strIdentify, sizeof(strIdentify), "%s:%u.%d",
							         oNodeReg.node_ip().c_str(), static_cast<unsigned>(oNodeReg.node_port()), j);
							expected[oNodeReg.node_type()].insert(strIdentify);
						}
					}

					for (const auto& pr : expected)
					{
						std::vector<std::string> locals;
						GetNodeIdentifys(pr.first, locals);
						for (const auto& id : locals)
						{
							if (pr.second.find(id) == pr.second.end())
							{
								DelNodeIdentify(pr.first, id);
							}
						}
					}

					for (const auto& pr : expected)
					{
						for (const auto& id : pr.second)
						{
							AddNodeIdentify(pr.first, id);
						}
					}
				}
			}
            else
            {
                LOG4_ERROR("%s() route mirror parse/read failed, keep old route. version=%llu bytes=%u",
                           __FUNCTION__,
                           static_cast<unsigned long long>(GetRouteNoticeVersionData().GetNodeNoticeVersion()),
                           GetRouteNoticeVersionData().GetNodeNoticeLength());
            }
		}
	}
	{//nodeid
		uint32 nodeid = GetRouteNoticeVersionData().GetNodeId();
		if (nodeid > 0 && GetNodeId() != nodeid)
		{
			LOG4_INFO("%s() SetNodeId:%u", __FUNCTION__,nodeid);
			SetNodeId(nodeid);
		}
	}
    {// custom config mirror
        if (GetCustomConfigVersionData().IsCustomVersionChange())
        {
            std::string customContent;
            if (GetCustomConfigVersionData().GetCustomConfig(customContent))
            {
                util::CJsonObject oCustomJson;
                if (oCustomJson.Parse(customContent))
                {
                    SetCustomConf(oCustomJson);
                    GetCustomConfigVersionData().UpdateCustomVersion();
                    LOG4_INFO("%s() custom mirror applied, version=%llu", __FUNCTION__,
                              static_cast<unsigned long long>(GetCustomConfigVersionData().GetCustomVersion()));
                }
                else
                {
                    LOG4_ERROR("%s() custom mirror parse failed, keep old custom. version=%llu, bytes=%u",
                               __FUNCTION__,
                               static_cast<unsigned long long>(GetCustomConfigVersionData().GetCustomVersion()),
                               GetCustomConfigVersionData().GetCustomLength());
                }
            }
            else
            {
                LOG4_ERROR("%s() custom mirror read failed. version=%llu, bytes=%u",
                           __FUNCTION__,
                           static_cast<unsigned long long>(GetCustomConfigVersionData().GetCustomVersion()),
                           GetCustomConfigVersionData().GetCustomLength());
            }
        }
    }
    return(true);
}

bool Worker::SendToParent(const MsgHead& oMsgHead,const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter = mapFdAttr.find(iManagerControlFd);
    if (iter != mapFdAttr.end())
    {
        int iErrno = 0;
        iter->second->pSendBuff->Write(oMsgHead.SerializeAsString().c_str(), oMsgHead.ByteSize());
        iter->second->pSendBuff->Write(oMsgBody.SerializeAsString().c_str(), oMsgBody.ByteSize());
        iter->second->pSendBuff->WriteFD(iManagerControlFd, iErrno);
        iter->second->pSendBuff->Compact(8192);
    }
    return(true);
}

bool Worker::IoRead(tagIoWatcherData* pData, struct ev_io* watcher)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (m_bWorkerReuseportAccept && watcher->fd == m_iC2SListenFd)
    {
        return(AcceptClientConn(watcher->fd));
    }
    else if (watcher->fd == iManagerDataFd)
    {
        return(FdTransfer());
    }
    else
    {
        return(RecvDataAndDispose(pData, watcher));
    }
}

bool Worker::RecvDataAndDispose(tagIoWatcherData* pData, struct ev_io* watcher)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    int iErrno = 0;
    int iReadLen = 0;
    auto conn_iter = mapFdAttr.find(pData->iFd);
    if (conn_iter == mapFdAttr.end())
    {
        LOG4_ERROR("no fd attr for %d!", pData->iFd);
    }
    else
    {
    	tagConnectionAttr* pConn = conn_iter->second.get();
    	pConn->dActiveTime = ev_now(m_loop);
        if (pData->ulSeq != pConn->ulSeq)
        {
            LOG4_TRACE("callback seq %u not match the conn attr seq %u",pData->ulSeq, pConn->ulSeq);
            DelEvent(watcher,pData);
            return(false);
        }
        pConn->pRecvBuff->Compact(8192);
        read_again:
        iReadLen = pConn->pRecvBuff->ReadFD(pData->iFd, iErrno);
		LOG4_TRACE("recv from fd %d ip %s identify %s, data len %d codec %d",pData->iFd, pConn->szRemoteAddr,pConn->strIdentify.c_str(),iReadLen, pConn->eCodecType);
        if (iReadLen > 0)
        {
            iRecvByte += iReadLen;
            MsgHead oInMsgHead, oOutMsgHead;
            MsgBody oInMsgBody, oOutMsgBody;
            auto codec_iter = mapCodec.find(conn_iter->second->eCodecType);
            if (codec_iter == mapCodec.end())
            {
                LOG4_ERROR("no codec found for %d!", conn_iter->second->eCodecType);
                DestroyConnect(conn_iter);
                return(false);
            }
            ThunderCodec* pCodec = codec_iter->second.get();
            while ((pConn->eCodecType == util::CODEC_HTTPS && pConn->pRecvBuff->ReadableBytes() > 0)
                    || (pConn->eCodecType != util::CODEC_HTTPS && pConn->pRecvBuff->ReadableBytes() >= gc_uiAppMsgHeadSize))
            {
                oInMsgHead.Clear();
                oInMsgBody.Clear();

                E_CODEC_STATUS eCodecStatus = pCodec->Decode(pConn, oInMsgHead, oInMsgBody);
                if (pConn->eCodecType == util::CODEC_HTTPS && pConn->pSendBuff->ReadableBytes() > 0)
                {
                    conn_iter->second->pSendBuff->WriteFD(pData->iFd, iErrno);
                    conn_iter->second->pSendBuff->Compact(8192);
                }
//				#ifdef _DEBUG
//                if (conn_iter->second->eCodecType == util::CODEC_TEST)
//				{
//					if (CODEC_STATUS_OK == eCodecStatus)
//					{
//						oInMsgBody.set_body("ok");
//						SendTo(tagMsgShell(pConn->iFd,pConn->ulSeq),oInMsgHead,oInMsgBody);
//					}
//					else if (CODEC_STATUS_ERR == eCodecStatus)
//					{
//						DestroyConnect(conn_iter);
//						return(false);
//					}
//					else
//					{
//						break;  // 数据尚未接收完整
//					}
//					return(true);
//				}
//                //网关类型节点的连接初始化时支持协议编解码器的替换（支持的是websocket json 或websocket pb与http,private的替换） （目前正式版不做编译器切换支持）
//                if (eConnectStatus_init == pConn->ucConnectStatus)
//                {
//                    //网关默认配置websocket json(可修改为websocket pb)
//                    if (CODEC_STATUS_ERR == eCodecStatus &&
//                        (util::CODEC_WEBSOCKET_EX_JS == pConn->eCodecType || util::CODEC_WEBSOCKET_EX_PB == pConn->eCodecType))
//                    {
//                        //切换为http协议
//                        LOG4_TRACE("failed to decode for codec %d,switch to CODEC_HTTP",pConn->eCodecType);
//                        conn_iter->second->eCodecType = util::CODEC_HTTP;
//                        auto codec_iter = mapCodec.find(pConn->eCodecType);
//                        if (codec_iter == mapCodec.end())
//                        {
//                            LOG4_ERROR("no codec found for %d!", pConn->eCodecType);
//                            DestroyConnect(conn_iter);
//                            return(false);
//                        }
//                        eCodecStatus = codec_iter->second->Decode(pConn, oInMsgHead, oInMsgBody);
//                    }
//                    if (CODEC_STATUS_ERR == eCodecStatus && util::CODEC_HTTP == pConn->eCodecType)
//                    {
//                        //切换为私有协议编解码（与客户端通信协议） private pb
//                        LOG4_TRACE("failed to decode for codec %d,switch to CODEC_PRIVATE",pConn->eCodecType);
//                        conn_iter->second->eCodecType = util::CODEC_PRIVATE;
//                        auto codec_iter = mapCodec.find(pConn->eCodecType);
//                        if (codec_iter == mapCodec.end())
//                        {
//                            LOG4_ERROR("no codec found for %d!", pConn->eCodecType);
//                            DestroyConnect(conn_iter);
//                            return(false);
//                        }
//                        eCodecStatus = codec_iter->second->Decode(pConn, oInMsgHead, oInMsgBody);
//                    }
//                }
//				#endif

                if (CODEC_STATUS_OK == eCodecStatus)
                {
                	pConn->ulMsgNumUnitTime++;      // 这里要做发送消息频率限制
					pConn->ulMsgNum++;
                    ++iRecvNum;
                    pConn->dActiveTime = GetTimeStamp();//ev_now(m_loop);
                    bool bDisposeResult = false;
                    // 基于TCP的自定义协议请求或带cmd、seq自定义头域的请求.http短连接不需要带cmd
                    if (oInMsgHead.cmd() > 0)
                    {
                    	if (util::CODEC_APP == pConn->eCodecType && IsAccess())
                    	{
							if (!pConn->IsVerify() && pConn->ulMsgNum > 1)   // 未经账号验证的客户端连接发送数据过来，直接断开
							{
								LOG4_WARN("invalid request, please login first!");
								DestroyConnect(conn_iter);
								return(false);
							}
                    	}
                    	LOG4_TRACE("%s() dMsgStatInterval(%lf) eCodecType(%d) cmd(%u)",__FUNCTION__,m_dMsgStatInterval,pConn->eCodecType,oInMsgHead.cmd());
                    	//网关对客户端上传的消息限流
                    	if (m_dMsgStatInterval > 0.0 && (gc_uiCmdReq & oInMsgHead.cmd())
                    			&& (util::CODEC_APP == pConn->eCodecType || util::CODEC_WEBSOCKET_EX_PB_APP == pConn->eCodecType))//CMD_REQ_BEAT != oInMsgHead.cmd()
                    	{
                    		if (pConn->dActiveTime > (pConn->dUnitLimitLastTime + m_dMsgStatInterval))
							{
								pConn->ulMsgNumUnitTime = 1;
								pConn->mapCmdsUnitMsgCounter.clear();
								pConn->dUnitLimitLastTime = pConn->dActiveTime;
							}
							pConn->mapCmdsUnitMsgCounter[oInMsgHead.cmd()]++;//指令统计，限流处理在逻辑层
							LOG4_TRACE("%s() cmd(%u) CmdsUnitMsgCounter(%u)",__FUNCTION__,oInMsgHead.cmd(),pConn->mapCmdsUnitMsgCounter[oInMsgHead.cmd()]);
							LOG4_TRACE("%s() iMsgPermitNum(%d) ulMsgNumUnitTime(%u) dUnitLimitLastTime(%lf)  dActiveTime(%lf) Millsecond(%llu)",
									__FUNCTION__,m_iMsgPermitNum,pConn->ulMsgNumUnitTime,pConn->dUnitLimitLastTime,pConn->dActiveTime,util::GetMillsecond());
							if (m_iMsgPermitNum > 0 && pConn->ulMsgNumUnitTime > m_iMsgPermitNum)
							{
								LOG4_WARN("%s() ulMsgNumUnitTime(%u) > m_iMsgPermitNum(%d)!",__FUNCTION__,pConn->ulMsgNumUnitTime,m_iMsgPermitNum);
								return(false);
							}
                    	}

                        bDisposeResult = Dispose(pConn, oInMsgHead, oInMsgBody, oOutMsgHead, oOutMsgBody); // 处理过程有可能会断开连接，所以下面要做连接是否存在检查
                        auto dispose_conn_iter = mapFdAttr.find(pData->iFd);
                        if (dispose_conn_iter == mapFdAttr.end() || pData->ulSeq != dispose_conn_iter->second->ulSeq)     // 连接已断开，资源已回收
                        {
                            return(true);
                        }
                        if (oOutMsgHead.ByteSize() > 0)
                        {
                            eCodecStatus = EncodeByConnectionCodec(pConn, pCodec, oOutMsgHead, oOutMsgBody, pConn->pSendBuff.get());
                            if (CODEC_STATUS_OK == eCodecStatus)
                            {
                                conn_iter->second->pSendBuff->WriteFD(pData->iFd, iErrno);
                                conn_iter->second->pSendBuff->Compact(8192);
                                if (iErrno != 0)
                                {
                                    LOG4_ERROR("error %d %s!",iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                                }
                            }
                            else if (CODEC_STATUS_ERR == eCodecStatus)
                            {
                                LOG4_ERROR("faild to Encode");
                            }
                        }
                    }
                    else  // url方式的http请求或者websocket
                    {
                    	if (pConn->IsVerify())
						{//长连接并且验证过的，不需要再验证。只填充附带数据。
							oInMsgBody.set_additional(pConn->pClientData->GetRawReadBuffer(),pConn->pClientData->ReadableBytes());
							oInMsgHead.set_msgbody_len(oInMsgBody.ByteSize());
						}
						else
						{
							// 如果是websocket连接，则需要验证连接
//							if (util::CODEC_WEBSOCKET_EX_PB_APP == pConn->eCodecType ||
//									util::CODEC_WEBSOCKET_EX_PB == pConn->eCodecType || util::CODEC_WEBSOCKET_EX_JS == pConn->eCodecType)
//							{//暂时不检验WEBSOCKET的账号验证
//								auto http_iter = mapHttpAttr.find(pConn->iFd);
//								if (http_iter == mapHttpAttr.end() && pConn->ulMsgNum > 1)   // 未经握手的websocket客户端连接发送数据过来，直接断开
//								{
//									LOG4_WARN("invalid request, please handshake first!");
//									DestroyConnect(conn_iter);
//									return(false);
//								}
//							}
//							else if (util::CODEC_HTTP == pConn->eCodecType)//其他类型长连接(目前对外开放的只有http和websocket)
//							{//暂时不检验http的账号验证
//								auto inner_iter = mapHttpAttr.find(pConn->iFd);
//								if (inner_iter == mapHttpAttr.end() && pConn->ulMsgNum > 1)   // 未经账号验证的客户端连接发送数据过来，直接断开
//								{
//									LOG4_WARN("invalid request, please login first!");
//									DestroyConnect(conn_iter);
//									return(false);
//								}
//							}
						}

                        HttpMsg oInHttpMsg;
                        HttpMsg oOutHttpMsg;
                        if (oInHttpMsg.ParseFromString(oInMsgBody.body()))
                        {
                            if (util::CODEC_HTTP == pConn->eCodecType || util::CODEC_HTTPS == pConn->eCodecType)
                            {
                            	pConn->dKeepAlive = 10;   // 未带KeepAlive参数的http协议，默认10秒钟关闭
                                LOG4_TRACE("set dKeepAlive(%lf)",pConn->dKeepAlive);
                            }
                            else
                            {
                                LOG4_TRACE("set dKeepAlive(%lf)",pConn->dKeepAlive);//websocket保持长连接,dKeepAlive为0
                            }

                            auto iter = oInHttpMsg.headers().find(std::string("Keep-Alive"));
                            if (iter != oInHttpMsg.headers().end())
                            {
                            	pConn->dKeepAlive = strtoul(iter->second.c_str(), NULL, 10);
								LOG4_TRACE("set dKeepAlive(%lf)",pConn->dKeepAlive);
								AddIoTimeout(pConn->iFd, pConn->ulSeq, pConn, pConn->dKeepAlive);
                            }
                            else
                            {
                            	auto iter = oInHttpMsg.headers().find(std::string("Connection"));
                            	if (iter != oInHttpMsg.headers().end())
                            	{
                            		if (std::string("keep-alive") == iter->second)
									{
                            			pConn->dKeepAlive = 65.0;
										LOG4_TRACE("set dKeepAlive(%lf)",pConn->dKeepAlive);
										AddIoTimeout(pConn->iFd, pConn->ulSeq, pConn, 65.0);//conn_iter->first
									}
									else if (std::string("close") == iter->second && HTTP_RESPONSE == oInHttpMsg.type())
									{   // 作为客户端请求远端http服务器，对方response后要求客户端关闭连接
										pConn->dKeepAlive = -1;
										LOG4_TRACE("std::string(\"close\") == iter->second");
										DestroyConnect(conn_iter);
									}
									else
									{
										AddIoTimeout(pConn->iFd, pConn->ulSeq, pConn, pConn->dKeepAlive);
									}
                            	}
                                else if ((util::CODEC_HTTP == pConn->eCodecType || util::CODEC_HTTPS == pConn->eCodecType)
                                                && pConn->dKeepAlive > 0)
                                {
                                    // 无 Keep-Alive / Connection 时，上面未刷新定时器；accept 阶段 1s 检测仍在，
                                    // 异步 Step/协程回包前会误关连接（curl 52 Empty reply）
                                    AddIoTimeout(pConn->iFd, pConn->ulSeq, pConn, pConn->dKeepAlive);
                                }
                            }

                            bDisposeResult = Dispose(pConn, oInHttpMsg, oOutHttpMsg);
                            // 处理过程有可能会断开连接，所以下面要做连接是否存在检查
							auto dispose_conn_iter = mapFdAttr.find(pData->iFd);
							if (dispose_conn_iter == mapFdAttr.end() || pData->ulSeq != dispose_conn_iter->second->ulSeq)     // 连接已断开，资源已回收
							{
								return(true);
							}

                            if (pConn->dKeepAlive < 0)
                            {
                                if (HTTP_RESPONSE == oInHttpMsg.type())
                                {
                                    LOG4_TRACE("if (HTTP_RESPONSE == oInHttpMsg.type())");
                                    DestroyConnect(conn_iter);
                                }
                                else
                                {
                                    ((HttpCodec*)pCodec)->AddHttpHeader("Connection", "close");
                                }
                            }
                            if (oOutHttpMsg.ByteSize() > 0)
                            {
                                eCodecStatus = EncodeByConnectionCodec(pConn, pCodec, oOutHttpMsg, pConn->pSendBuff.get());
                                if (CODEC_STATUS_OK == eCodecStatus)
                                {
                                    conn_iter->second->pSendBuff->WriteFD(pData->iFd, iErrno);
                                    conn_iter->second->pSendBuff->Compact(8192);
                                    if (iErrno != 0)
                                    {
                                        LOG4_ERROR("error %d %s!",iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                                    }
                                }
                                else if (CODEC_STATUS_ERR == eCodecStatus)
                                {
                                    LOG4_ERROR("faild to Encode");
                                }
                            }
                        }
                        else
                        {
                            LOG4_ERROR("oInHttpMsg.ParseFromString() error!");
                        }
                    }
                    if (!bDisposeResult)
                    {
                        break;
                    }
                }
                else if (CODEC_STATUS_ERR == eCodecStatus)
                {
//                    if (pData->iFd != iManagerControlFd && pData->iFd != iManagerDataFd)
//                    {
//                        LOG4_TRACE("if (pData->iFd != iManagerControlFd && pData->iFd != iManagerDataFd)");
                		LOG4_TRACE("CODEC_STATUS_ERR DestroyConnect iFd(%d)",pData->iFd);
                        DestroyConnect(conn_iter);
//                    }
                    return(false);
                }
                else
                {
                    break;  // 数据尚未接收完整
                }
            }
            return(true);
        }
        else if (iReadLen == 0)
        {
            LOG4_TRACE("fd %d closed by peer, errno %d %s!",pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
            DestroyConnect(conn_iter);
        }
        else
        {
            LOG4_TRACE("recv from fd %d errno %d: %s",pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
            if (EAGAIN == iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
            {
                ;
            }
            else if (EINTR == iErrno)//中断继续读
            {
                goto read_again;
            }
            else
            {
                LOG4_ERROR("recv from fd %d errno %d: %s",pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                DestroyConnect(conn_iter);
            }
        }
    }
    return(false);
}

bool Worker::FdTransfer()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    char szIpAddr[16] = {0};
    int iCodec = 0;
    int iAcceptFd = recv_fd_with_attr(iManagerDataFd, szIpAddr, 16, &iCodec);
    if (iAcceptFd <= 0)
    {
        if (iAcceptFd == 0)//父进程关闭后，子进程收到的消息
        {
            LOG4_ERROR("recv_fd from iManagerDataFd %d len:%d", iManagerDataFd, iAcceptFd);
            Destroy();
            exit(2); // manager与worker通信fd已关闭，worker进程退出
        }
        else //if (errno != EAGAIN)
        {
        	if (iAcceptFd == -2)
        	{
        		//文件描述符传送失败的，等待重试（有可能是系统参数导致）,为了不影响已有连接，暂时不重启
        		LOG4_ERROR("recv_fd from iManagerDataFd %d error %d:%s ret:%d.wait for retry", iManagerDataFd,errno,strerror_r(errno, m_pErrBuff, gc_iErrBuffLen),iAcceptFd);
        		return(false);
        	}
        	LOG4_ERROR("recv_fd from iManagerDataFd %d error %d:%s ret:%d", iManagerDataFd,errno,strerror_r(errno, m_pErrBuff, gc_iErrBuffLen),iAcceptFd);
			Destroy();
			exit(2); // manager与worker通信fd已关闭，worker进程退出
        }
    }
    else
    {
    	tagConnectionAttr* pConnAttr = CreateAcceptFdAttr(iAcceptFd,GetFdSequence(),util::E_CODEC_TYPE(iCodec));
        if (pConnAttr)
        {
            snprintf(pConnAttr->szRemoteAddr, 32, "%s", szIpAddr);
            LOG4_TRACE("pConnAttr->szRemoteAddr = %s, iCodec = %d", pConnAttr->szRemoteAddr, iCodec);
        }
    }
    return(false);
}

bool Worker::AcceptClientConn(int iFd)
{
    struct sockaddr_in stClientAddr;
    socklen_t clientAddrSize = sizeof(stClientAddr);
    int iAcceptFd = accept(iFd, (struct sockaddr*) &stClientAddr, &clientAddrSize);
    if (iAcceptFd < 0)
    {
        LOG4_ERROR("accept error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        return(false);
    }
    SetSocketAttr(iAcceptFd,false);//网关对外的连接不使用探测包
    tagConnectionAttr* pConnAttr = CreateAcceptFdAttr(iAcceptFd, GetFdSequence(), m_eAccessCodec);
    if (pConnAttr == nullptr)
    {
        LOG4_ERROR("CreateAcceptFdAttr failed for client fd %d", iAcceptFd);
        close(iAcceptFd);
        return(false);
    }
    snprintf(pConnAttr->szRemoteAddr, sizeof(pConnAttr->szRemoteAddr), "%s", inet_ntoa(stClientAddr.sin_addr));
    return true;
}

bool Worker::IoWrite(tagIoWatcherData* pData, struct ev_io* watcher)
{
    //LOG4_TRACE("%s()", __FUNCTION__);
    auto attr_iter =  mapFdAttr.find(pData->iFd);
    if (attr_iter == mapFdAttr.end())
    {
        return(false);
    }
    else
    {
    	tagConnectionAttr* pConn = attr_iter->second.get();
        if (pData->ulSeq != pConn->ulSeq || pData->iFd != pConn->iFd)
        {
            LOG4_TRACE("callback seq %u iFd %d not match the conn attr seq %u ifd %d",
                            pData->ulSeq, pData->iFd,attr_iter->second->ulSeq,pConn->iFd);
            DelEvent(watcher,pData);
            return(false);
        }
        int iErrno = 0;
        int iNeedWriteLen = (int)attr_iter->second->pSendBuff->ReadableBytes();
        int iWriteLen = pConn->pSendBuff->WriteFD(pData->iFd, iErrno);
        pConn->pSendBuff->Compact(8192);
        if (iWriteLen < 0)
        {
            if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
            {
                LOG4_TRACE("if (EAGAIN != iErrno && EINTR != iErrno)");
                LOG4_ERROR("send to fd %d error %d: %s",pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                DestroyConnect(attr_iter);
            }
            else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
            {
                attr_iter->second->dActiveTime = ev_now(m_loop);
                AddIoWriteEvent(pConn);
            }
        }
        else if (iWriteLen > 0)
        {
            iSendByte += iWriteLen;
            pConn->dActiveTime = ev_now(m_loop);
			if (iWriteLen == iNeedWriteLen)  // 数据已写完，取消监听fd写事件
            {
                RemoveIoWriteEvent(pConn);
            }
            else    // 内容未写完，添加或保持监听fd写事件
            {
                AddIoWriteEvent(pConn);
            }
        }
        else    // iWriteLen == 0 写缓冲区为空
        {
//            LOG4_TRACE("pData->iFd %d, watcher->fd %d, iter->second->pWaitForSendBuff->ReadableBytes()=%d",pData->iFd, watcher->fd, attr_iter->second->pWaitForSendBuff->ReadableBytes());
            if (pConn->pWaitForSendBuff->ReadableBytes() > 0)    // 存在等待发送的数据，说明本次写事件是connect之后的第一个写事件
            {
            	tagMsgShell stMsgShell(pData->iFd,attr_iter->second->ulSeq);
                auto index_iter = mapSeq2WorkerIndex.find(attr_iter->second->ulSeq);
                if (index_iter != mapSeq2WorkerIndex.end())   // 系统内部Server间通信需由Manager转发
                {
                    AddInnerFd(stMsgShell);
                    if (util::CODEC_PB_INTERNAL == pConn->eCodecType)  // 系统内部Server间通信
                    {
                    	CmdConnectWorker::Start(stMsgShell, index_iter->second);
                    }
                    else        // 与系统外部Server通信，连接成功后直接将数据发送
                    {
                        SendTo(stMsgShell);
                    }
                    mapSeq2WorkerIndex.erase(index_iter);
                    LOG4_TRACE("RemoveIoWriteEvent(%d)", pData->iFd);
                    RemoveIoWriteEvent(pConn);    // 在m_pCmdConnect的两个回调之后再把等待发送的数据发送出去
                }
                else // 与系统外部Server通信，连接成功后直接将数据发送
                {
                    SendTo(stMsgShell);
                }
            }
        }
        return(true);
    }
}

bool Worker::IoError(tagIoWatcherData* pData, struct ev_io* watcher)
{
    //LOG4_TRACE("%s()", __FUNCTION__);
    auto iter =  mapFdAttr.find(pData->iFd);
    if (iter == mapFdAttr.end())
    {
        return(false);
    }
    else
    {
        LOG4_TRACE("if (iter == mapFdAttr.end()) else");
        if (pData->ulSeq != iter->second->ulSeq)
        {
            LOG4_TRACE("callback seq %u not match the conn attr seq %u",pData->ulSeq, iter->second->ulSeq);
            DelEvent(watcher,pData);
            return(false);
        }
        DestroyConnect(iter);
        return(true);
    }
}

bool Worker::IoTimeout(struct ev_timer* watcher, bool bCheckBeat)
{
    LOG4_TRACE("%s()",__FUNCTION__);
    bool bRes = false;
    tagIoWatcherData* pData = (tagIoWatcherData*)watcher->data;
    if (pData == nullptr)
    {
        LOG4_ERROR("pData is null in %s()", __FUNCTION__);
        DelEvent(watcher);
        SAFE_DELETE(watcher);
        return(false);
    }
    auto iter =  mapFdAttr.find(pData->iFd);
    if (iter == mapFdAttr.end())
    {
        LOG4_TRACE("%s() iFd(%d) not found",__FUNCTION__,pData->iFd);
        bRes = false;
    }
    else
    {
    	tagConnectionAttr* pConn = iter->second.get();
        if (pConn->ulSeq != pData->ulSeq)      // 文件描述符数值相等，但已不是原来的文件描述符
        {
            LOG4_TRACE("%s() ulSeq(%u) not found",__FUNCTION__,pData->ulSeq);
            bRes = false;
        }
        else
        {
            LOG4_TRACE("%s() eCodecType(%d) ev_now(%lf) IoTimeout(%lf) bCheckBeat(%d) dKeepAlive(%lf) szRemoteAddr(%s) strIdentify(%s)",
				__FUNCTION__,pConn->eCodecType,ev_now(m_loop),m_dIoTimeout,bCheckBeat,pConn->dKeepAlive,pConn->szRemoteAddr,pConn->strIdentify.c_str());
            if (util::CODEC_PB_INTERNAL != pConn->eCodecType)//外部长连接需要检查非校验连接超时回收（被动连接调用）
			{
				LOG4_TRACE("IoTimeout pConn(%d,%d,%u) IsVerify(%d)",pConn->eCodecType,pConn->iFd,pConn->ulSeq,pConn->IsVerify());
				if (pConn->dKeepAlive == 0)
				{
					if (pConn->IsVerify())
					{
						return(true);//已校验的外部连接则不需要再检查
					}
					else
					{
						if (m_iAccessVerifyTime > 0)//需要检查长连接校验的
						{
							ev_tstamp after = pConn->dActiveTime - ev_now(m_loop) + m_iAccessVerifyTime;
							if (after > 0)    // IO在定时时间内被重新刷新过，重新设置定时器
							{
								LOG4_TRACE("RefreshEvent after(%lf) dActiveTime(%lf) ev_now(%lf) iAccessVerifyTime(%u)",after,pConn->dActiveTime,ev_now(m_loop),m_iAccessVerifyTime);
								RefreshEvent(after,watcher);
								return(true);
							}
							LOG4_WARN("io timeout and Destroy Connect!dActiveTime(%lf) ev_now(%lf) iAccessVerifyTime(%u)",pConn->dActiveTime,ev_now(m_loop),m_iAccessVerifyTime);
							DestroyConnect(iter);
							return(false);
						}
					}
				}
				else if (pConn->dKeepAlive > 0)
				{
					ev_tstamp after = pConn->dActiveTime - ev_now(m_loop) + pConn->dKeepAlive;
					if (after > 0)    // IO在定时时间内被重新刷新过，重新设置定时器
					{
						LOG4_TRACE("RefreshEvent after(%lf) dActiveTime(%lf) ev_now(%lf) dKeepAlive(%lf)",after,pConn->dActiveTime,ev_now(m_loop),pConn->dKeepAlive);
						RefreshEvent(after,watcher);
						return(true);
					}
					LOG4_WARN("io timeout and Destroy Connect!dActiveTime(%lf) ev_now(%lf) dKeepAlive(%lf)",pConn->dActiveTime,ev_now(m_loop),pConn->dKeepAlive);
					DestroyConnect(iter);
					return(false);
				}
				else
				{
					LOG4_WARN("Destroy Connect!dActiveTime(%lf) ev_now(%lf) dKeepAlive(%lf)",pConn->dActiveTime,ev_now(m_loop),pConn->dKeepAlive);
					DestroyConnect(iter);
					return(false);
				}
			}
            else//内部连接处理
            {
            	if (pConn->dKeepAlive == 0)     // 长连接需要发送心跳检查
				{
					tagMsgShell stMsgShell(pData->iFd,pConn->ulSeq);
					LOG4_TRACE("CMD_REQ_BEAT strIdentify(%s) dIoTimeout(%lf) stMsgShell(%d,%u) bCheckBeat(%d) pConn(%d,%d,%u) szRemoteAddr(%s)",
							pConn->strIdentify.c_str(),m_dIoTimeout,stMsgShell.iFd,stMsgShell.ulSeq,bCheckBeat,pConn->eCodecType,pConn->iFd,
							pConn->ulSeq,pConn->szRemoteAddr);
					SendTo(stMsgShell, CMD_REQ_BEAT,GetSequence(),"");//内部连接保活
					RefreshEvent(m_dIoTimeout,watcher);//重新设置定时器
					CheckHeartBeat(stMsgShell);
					return(true);
					//内部长连接才主动发送心跳检查来检查连接状态（主动连接调用）
//					ev_tstamp after = pConn->dActiveTime - ev_now(m_loop) + m_dIoTimeout;
//					if (after > 0)    // IO在定时时间内被重新刷新过，重新设置定时器
//					{
//						LOG4_TRACE("RefreshEvent after(%lf) dActiveTime(%lf) ev_now(%lf) dIoTimeout(%lf)",after,pConn->dActiveTime,ev_now(m_loop),m_dIoTimeout);
//						RefreshEvent(after,watcher);
//						return(true);
//					}
//					else    // IO已超时，发送心跳检查
//					{
//						tagMsgShell stMsgShell(pData->iFd,pConn->ulSeq);
//						LOG4_TRACE("stMsgShell(%d,%u) bCheckBeat(%d) IoTimeout pConn(%d,%d,%u) szRemoteAddr(%s) strIdentify(%s)",
//								stMsgShell.iFd,stMsgShell.ulSeq,bCheckBeat,pConn->eCodecType,pConn->iFd,
//								pConn->ulSeq,pConn->szRemoteAddr,pConn->strIdentify.c_str());
////						ExecStep(new StepIoTimeout(tagMsgShell(pData->iFd,pConn->ulSeq), watcher),3.0);//发送心跳包
//						SendTo(stMsgShell, CMD_REQ_BEAT,GetSequence(),"");
//					}
				}
            }
            bRes = true;
        }
    }
    return(bRes);
}

bool Worker::StepTimeout(Step* pStep, struct ev_timer* watcher)
{
    ev_tstamp after = pStep->GetActiveTime() - ev_now(m_loop) + pStep->GetTimeout();
    if (after > 0)    // 在定时时间内被重新刷新过，重新设置定时器
    {
        RefreshEvent(after,watcher);
        LOG4_TRACE("%s(pStep %p seq %u): reset watcher %lf",__FUNCTION__,pStep, pStep->GetSequence(), after + ev_time() - ev_now(m_loop));
        return(true);
    }
    else    // 会话已超时
    {
        LOG4_TRACE("%s(pStep %p seq %u): active_time %lf, now_time %lf, lifetime %lf",
                        __FUNCTION__,pStep, pStep->GetSequence(), pStep->GetActiveTime(), ev_now(m_loop), pStep->GetTimeout());
        E_CMD_STATUS eResult = pStep->Timeout();
        if (STATUS_CMD_RUNNING == eResult)      // 超时Timeout()里重新执行Start()，重新设置定时器
        {
            RefreshEvent(pStep->GetTimeout(),watcher);
            return(true);
        }
        else
        {
            DeleteCallback(pStep);
            return(true);
        }
    }
}

bool Worker::SessionTimeout(Session* pSession, struct ev_timer* watcher)
{
    ev_tstamp after = pSession->GetActiveTime() - ev_now(m_loop) + pSession->GetTimeout();
    if (after > 0)    // 定时时间内被重新刷新过，重新设置定时器
    {
    	RefreshEvent(after,watcher);
        return(true);
    }
    else    // 会话已超时
    {
        //LOG4_TRACE("%s(session_name: %s,  session_id: %s)",__FUNCTION__, pSession->GetSessionClass().c_str(), pSession->GetSessionId().c_str());
        if (STATUS_CMD_RUNNING == pSession->Timeout())   // 定时器时间到，执行定时操作，session时间刷新
        {
        	RefreshEvent(pSession->GetTimeout(),watcher);
            return(true);
        }
        else
        {
            DeleteCallback(pSession);
            return(true);
        }
    }
}

bool Worker::OnRedisConnect(const redisAsyncContext *c, int status)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto attr_iter = mapRedisAttr.find((redisAsyncContext*)c);
    if (attr_iter != mapRedisAttr.end())
    {
        if (status == REDIS_OK)
        {
            if (false == attr_iter->second->bIsReady && attr_iter->second->strPassword.size() > 0)
            {
            	net::RedisOperator oRedisOperator(0, attr_iter->second->strPassword,"","AUTH");//"AUTH %s", redis_password
            	StepAuthRedis* pStepAuthRedis = new StepAuthRedis(oRedisOperator.MakeMemOperate()->redis_operate(),attr_iter->second);
            	attr_iter->second->listWaitData.push_front(std::unique_ptr<RedisStep>(static_cast<RedisStep*>(pStepAuthRedis)));
            }
            else
            {
            	attr_iter->second->bIsReady = true;//异步连接的后续处理
            }
            int iCmdStatus;
            for (auto step_iter = attr_iter->second->listWaitData.begin();
                            step_iter != attr_iter->second->listWaitData.end(); )
            {
                RedisStep* pRedisStep = step_iter->get();

                if (pRedisStep->RedisCmd()->GetRawCmds().size() > 0)
				{//批处理指令
                	bool boContinue(true);
                	bool boPush(false);
					const std::vector<std::string>& rawCmds = pRedisStep->RedisCmd()->GetRawCmds();
					for(int i = 0;i < rawCmds.size() ;++i)
					{
						int status = redisAsyncCommand((redisAsyncContext*)c, RedisCmdCallback, NULL, rawCmds[i].c_str());
						if (status == REDIS_OK)
						{
							LOG4_TRACE("bulk:succeed in sending redis cmd(%s)", rawCmds[i].c_str());
							if (!boPush)
							{
								boPush = true;
								attr_iter->second->listData.push_back(std::move(*step_iter));
							}
							pRedisStep->AddSendCounter();
						}
						else
						{
							LOG4_ERROR("bulk:failed to sending redis cmd:(%s).status(%d)", rawCmds[i].c_str(), status);
							boContinue = false;
							break;
						}
					}
					attr_iter->second->listWaitData.erase(step_iter++);
					if (!boContinue)
					{//等待下一次回调
						break;
					}
				}
                else
                {
                	size_t args_size = pRedisStep->GetRedisCmd()->GetCmdArguments().size() + 1;
					const char* argv[args_size];
					size_t arglen[args_size];
					argv[0] = pRedisStep->GetRedisCmd()->GetCmd().c_str();
					arglen[0] = pRedisStep->GetRedisCmd()->GetCmd().size();
					std::vector<std::pair<std::string, bool> >::const_iterator c_iter = pRedisStep->GetRedisCmd()->GetCmdArguments().begin();
					for (size_t i = 1; c_iter != pRedisStep->GetRedisCmd()->GetCmdArguments().end(); ++c_iter, ++i)
					{
						argv[i] = c_iter->first.c_str();
						arglen[i] = c_iter->first.size();
					}
					iCmdStatus = redisAsyncCommandArgv((redisAsyncContext*)c, RedisCmdCallback, NULL, args_size, argv, arglen);
					if (iCmdStatus == REDIS_OK)
					{
						LOG4_TRACE("succeed in sending redis cmd: %s", pRedisStep->GetRedisCmd()->ToString().c_str());
						attr_iter->second->listData.push_back(std::move(*step_iter));
						attr_iter->second->listWaitData.erase(step_iter++);
						pRedisStep->AddSendCounter();
					}
					else    // 命令执行失败，不再继续执行，等待下一次回调
					{
						break;
					}
                }
            }
        }
        else
        {//连接失败则放弃步骤
            for (auto step_iter = attr_iter->second->listWaitData.begin();
                            step_iter != attr_iter->second->listWaitData.end(); ++step_iter)
            {
                step_iter->get()->Callback(c, status, NULL);
            }
            attr_iter->second->listWaitData.clear();
            SAFE_DELETE(attr_iter->second);
            DelRedisContextAddr(c);
            mapRedisAttr.erase(attr_iter);
        }
    }
    return(true);
}

bool Worker::OnRedisDisconnect(const redisAsyncContext *c, int status)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto attr_iter = mapRedisAttr.find((redisAsyncContext*)c);
    if (attr_iter != mapRedisAttr.end())
    {//连接断开则放弃步骤
        for (auto& step_iter : attr_iter->second->listData)
        {
            LOG4_ERROR("OnRedisDisconnect callback error %d of redis cmd: %s",
                            c->err, step_iter->GetRedisCmd()->ToString().c_str());
            step_iter->Callback(c, c->err, NULL);
        }
        attr_iter->second->listData.clear();

        for (auto&  step_iter : attr_iter->second->listWaitData)
        {
            LOG4_ERROR("OnRedisDisconnect callback error %d of redis cmd: %s",
                            c->err, step_iter->GetRedisCmd()->ToString().c_str());
            step_iter->Callback(c, c->err, NULL);
        }
        attr_iter->second->listWaitData.clear();

        SAFE_DELETE(attr_iter->second);
        DelRedisContextAddr(c);
        mapRedisAttr.erase(attr_iter);
    }
    return(true);
}

bool Worker::OnRedisCmdResult(redisAsyncContext *c, void *reply, void *privdata)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    std::unordered_map<redisAsyncContext*, tagRedisAttr*>::iterator attr_iter = mapRedisAttr.find((redisAsyncContext*)c);
    if (attr_iter != mapRedisAttr.end())
    {
        auto step_iter = attr_iter->second->listData.begin();
        if (nullptr == reply)
        {//结果失效，放弃连接
            std::unordered_map<const redisAsyncContext*, std::string>::iterator identify_iter = mapContextIdentify.find(c);
            if (identify_iter != mapContextIdentify.end())
            {
                LOG4_ERROR("redis %s error %d: %s", identify_iter->second.c_str(), c->err, c->errstr);
            }
            for ( ; step_iter != attr_iter->second->listData.end(); ++step_iter)
            {
            	RedisStep* pRedisStep = step_iter->get();
                LOG4_ERROR("callback error %d of redis cmd: %s", c->err, pRedisStep->GetRedisCmd()->ToString().c_str());
                pRedisStep->Callback(c, c->err, (redisReply*)reply);
            }
            attr_iter->second->listData.clear();
            SAFE_DELETE(attr_iter->second);
            DelRedisContextAddr(c);
            mapRedisAttr.erase(attr_iter);
        }
        else
        {
            if (step_iter != attr_iter->second->listData.end())
            {
            	RedisStep* pRedisStep = step_iter->get();
                LOG4_TRACE("callback of redis cmd: %s", pRedisStep->GetRedisCmd()->ToString().c_str());
                /** @note 注意，若Callback返回STATUS_CMD_RUNNING，则该步骤需要继续进行 */
                pRedisStep->AddCallBackCounter();
                if (STATUS_CMD_RUNNING != pRedisStep->Callback(c, REDIS_OK, (redisReply*)reply))
                {
                	attr_iter->second->listData.erase(step_iter);
                }
                //freeReplyObject(reply);
            }
            else
            {
                LOG4_ERROR("no redis callback data found!");
            }
        }
    }
    return(true);
}

bool Worker::OnRedisClusterCmdResult(redisClusterAsyncContext *acc, void *reply, void *privdata)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    std::unordered_map<redisClusterAsyncContext*, tagRedisAttr*>::iterator attr_iter = mapRedisClusterAttr.find(acc);
    if (attr_iter != mapRedisClusterAttr.end())
    {
        LOG4_TRACE("%s() redisClusterAsyncContext(%s,%d)", __FUNCTION__,acc->cc->ip,acc->cc->port);
        redisAsyncContext cobj;//对逻辑层只是抛出错误信息，不直接使用连接对象
        cobj.err = acc->err;
        cobj.errstr = acc->errstr;
        redisAsyncContext *c = &cobj;
        auto step_iter = attr_iter->second->listData.begin();
        if (nullptr == reply)
        {//结果失效，放弃连接
            for ( ; step_iter != attr_iter->second->listData.end(); ++step_iter)
            {
            	RedisStep* pRedisStep = step_iter->get();
                LOG4_ERROR("callback error %d of redis cmd: %s", c->err, pRedisStep->GetRedisCmd()->ToString().c_str());
                pRedisStep->Callback(c, c->err, (redisReply*)reply);
            }
            attr_iter->second->listData.clear();

            SAFE_DELETE(attr_iter->second);
            mapRedisClusterAttr.erase(attr_iter);
        }
        else
        {
            if (step_iter != attr_iter->second->listData.end())
            {
            	RedisStep* pRedisStep = step_iter->get();
                LOG4_TRACE("callback of redis cmd: %s", pRedisStep->GetRedisCmd()->ToString().c_str());
                /** @note 注意，若Callback返回STATUS_CMD_RUNNING，则该步骤需要继续进行 */
                pRedisStep->AddCallBackCounter();
                if (STATUS_CMD_RUNNING != pRedisStep->Callback(c, REDIS_OK, (redisReply*)reply))
                {
                	attr_iter->second->listData.erase(step_iter);
                }
                //freeReplyObject(reply);
            }
            else
            {
                LOG4_ERROR("no redis callback data found!");
            }
        }
    }
    else
    {
        LOG4_WARN("%s() redisClusterAsyncContext(%s,%d) not found", __FUNCTION__,acc->cc->ip,acc->cc->port);
    }
    return(true);
}

const std::string& Worker::GetWorkerIdentify()
{
	if (m_strWorkerIdentify.size() == 0)
	{
		char szWorkerIdentify[64] = {0};
		snprintf(szWorkerIdentify, 64, "%s:%d.%d", GetHostForServer().c_str(),GetPortForServer(), GetWorkerIndex());
		m_strWorkerIdentify = szWorkerIdentify;
	}
	return(m_strWorkerIdentify);
}

bool Worker::RegisterCallback(Step* pStep, ev_tstamp dTimeout)
{
    LOG4_TRACE("%s(Step* 0x%p, lifetime %lf)", __FUNCTION__, pStep, dTimeout);
    if (pStep == nullptr)return(false);
    if (pStep->IsRegistered())  // 已注册过，不必重复注册，不过认为本次注册成功
    {
        LOG4_WARN("Step(0x%p,seq %u) registered already.",pStep,pStep->GetSequence());
        return(true);
    }
    pStep->SetRegistered();
    pStep->SetActiveTime(ev_now(m_loop));
    auto ret = mapCallbackStep.insert(std::make_pair(pStep->GetSequence(), std::unique_ptr<Step>(pStep)));
    if (ret.second)
    {
    	AddStep(pStep,dTimeout,StepTimeoutCallback);
        LOG4_TRACE("Step(0x%p,seq %u, active_time %lf, lifetime %lf) register successful.",pStep,
                        pStep->GetSequence(), pStep->GetActiveTime(), pStep->GetTimeout());
    }
    else
    {
        LOG4_WARN("Step(seq %u) register failed.",pStep->GetSequence());
    }
    return(ret.second);
}

void Worker::DeleteCallback(Step* pStep)
{
    LOG4_TRACE("%s(Step* %p)", __FUNCTION__, pStep);
    if (pStep == nullptr)
    {
        return;
    }
    DelEvent(pStep->m_pTimeoutWatcher);

    auto callback_iter = mapCallbackStep.find(pStep->GetSequence());
    if (callback_iter != mapCallbackStep.end())
    {
        LOG4_TRACE("delete step(seq %u)", pStep->GetSequence());
        mapCallbackStep.erase(callback_iter);
    }
}

bool Worker::RegisterCallback(Session* pSession)
{
    LOG4_TRACE("%s(Session* 0x%p, lifetime %lf)", __FUNCTION__, &pSession, pSession->GetTimeout());
    if (pSession == nullptr)
    {
        return(false);
    }
    if (pSession->IsRegistered())  // 已注册过，不必重复注册，不过认为本次注册成功
    {
        LOG4_WARN("Session(session_id %s) registered already.", pSession->GetSessionId().c_str());
        return(true);
    }
    pSession->SetRegistered();
    pSession->SetActiveTime(ev_now(m_loop));

    std::pair<std::unordered_map<std::string, Session*>::iterator, bool> ret;
    std::unordered_map<std::string, std::unordered_map<std::string, Session*> >::iterator name_iter = mapCallbackSession.find(pSession->GetSessionClass());
    if (name_iter == mapCallbackSession.end())
    {
        std::unordered_map<std::string, Session*> mapSession;
        ret = mapSession.insert(std::pair<std::string, Session*>(pSession->GetSessionId(), pSession));
        if(!ret.second)
        {
            LOG4_WARN("insert Session(session_id %s).SessionClass(%s) register failed.", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
        else
        {
            LOG4_TRACE("inserted Session(session_id %s).SessionClass(%s)", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
        mapCallbackSession.insert(std::pair<std::string, std::unordered_map<std::string, Session*> >(pSession->GetSessionClass(), mapSession));
    }
    else
    {
        LOG4_TRACE("try insert Session(session_id %s).SessionClass(%s)", pSession->GetSessionId().c_str(),
                        pSession->GetSessionClass().c_str());
        ret = name_iter->second.insert(std::pair<std::string, Session*>(pSession->GetSessionId(), pSession));
        if(!ret.second)
        {
            LOG4_WARN("insert Session(session_id %s).SessionClass(%s) register failed.", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
        else
        {
            LOG4_TRACE("inserted Session(session_id %s).SessionClass(%s)", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
    }
    if (ret.second)
    {
    	AddSession(pSession,pSession->GetTimeout(),SessionTimeoutCallback);
        LOG4_TRACE("Session(session_id %s) register successful.", pSession->GetSessionId().c_str());
    }
    else
    {
        LOG4_WARN("Session(session_id %s) register failed.", pSession->GetSessionId().c_str());
    }
    return(ret.second);
}

void Worker::DeleteCallback(Session* pSession)
{
    LOG4_TRACE("%s(Session* 0x%p)", __FUNCTION__, &pSession);
    if (pSession == nullptr)
    {
        return;
    }
    DelEvent(pSession->m_pTimeoutWatcher);
    auto name_iter = mapCallbackSession.find(pSession->GetSessionClass());
    if (name_iter != mapCallbackSession.end())
    {
        auto id_iter = name_iter->second.find(pSession->GetSessionId());
        if (id_iter != name_iter->second.end())
        {
            LOG4_TRACE("delete session(session_id %s)", pSession->GetSessionId().c_str());
            SAFE_DELETE(id_iter->second);
            name_iter->second.erase(id_iter);
        }
    }
}

bool Worker::RegisterCallback(const redisAsyncContext* pRedisContext, RedisStep* pRedisStep)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (pRedisStep == nullptr)
    {
        return(false);
    }
    if (pRedisStep->IsRegistered())  // 已注册过，不必重复注册，不过认为本次注册成功
    {
        return(true);
    }
    pRedisStep->SetRegistered();
    /* redis回调暂不作超时处理
    ev_timer* timeout_watcher = new ev_timer();
    if (timeout_watcher == nullptr)
    {
        return(false);
    }
    tagIoWatcherData* pData = new tagIoWatcherData();
    if (pData == nullptr)
    {
        LOG4_ERROR("new tagIoWatcherData error!");
        delete timeout_watcher;
        return(false);
    }
    ev_timer_init (timeout_watcher, IoTimeoutCallback, 0.5 + ev_time() - ev_now(m_loop), 0.0);
    pData->ullSeq = pStep->GetSequence();
    pData->pWorker = this;
    timeout_watcher->data = (void*)pData;
    ev_timer_start (m_loop, timeout_watcher);
    */

    auto iter = mapRedisAttr.find((redisAsyncContext*)pRedisContext);
    if (iter == mapRedisAttr.end())
    {
        LOG4_ERROR("redis attr not exist!");
        return(false);
    }
    else
    {
        if (iter->second->bIsReady)
        {
        	if (pRedisStep->RedisCmd()->GetRawCmds().size() > 0)
        	{//批处理指令
        		bool boPush(false);
				const std::vector<std::string>& rawCmds = pRedisStep->RedisCmd()->GetRawCmds();
				for(int i = 0;i < rawCmds.size() ;++i)
				{
					int status = redisAsyncCommand((redisAsyncContext*)pRedisContext, RedisCmdCallback, NULL, rawCmds[i].c_str());
					if (status == REDIS_OK)
					{
						LOG4_TRACE("bulk:succeed in sending redis cmd(%s)", rawCmds[i].c_str());
						if (!boPush)
						{
							boPush = true;
							iter->second->listData.push_back(std::unique_ptr<RedisStep>(pRedisStep));
						}
						pRedisStep->AddSendCounter();
					}
					else
					{
						LOG4_ERROR("bulk:failed to sending redis cmd:(%s).status(%d)", rawCmds[i].c_str(), status);
					}
				}
				return(true);
        	}
        	else
        	{//一般指令
        		int status;
				size_t args_size = pRedisStep->GetRedisCmd()->GetCmdArguments().size() + 1;
				const char* argv[args_size];
				size_t arglen[args_size];
				argv[0] = pRedisStep->GetRedisCmd()->GetCmd().c_str();
				arglen[0] = pRedisStep->GetRedisCmd()->GetCmd().size();
				std::vector<std::pair<std::string, bool> >::const_iterator c_iter = pRedisStep->GetRedisCmd()->GetCmdArguments().begin();
				for (size_t i = 1; c_iter != pRedisStep->GetRedisCmd()->GetCmdArguments().end(); ++c_iter, ++i)
				{
					argv[i] = c_iter->first.c_str();
					arglen[i] = c_iter->first.size();
				}
				status = redisAsyncCommandArgv((redisAsyncContext*)pRedisContext, RedisCmdCallback, NULL, args_size, argv, arglen);
				if (status == REDIS_OK)
				{
					LOG4_TRACE("succeed in sending redis cmd: %s", pRedisStep->GetRedisCmd()->ToString().c_str());
					iter->second->listData.push_back(std::unique_ptr<RedisStep>(pRedisStep));
					pRedisStep->AddSendCounter();
					return(true);
				}
				else
				{
					LOG4_ERROR("redis status %d!", status);
					return(false);
				}
        	}
        }
        else
        {
        	LOG4_TRACE("iter->second->bIsReady = %d iter->second->bIsAuthFail = %d", iter->second->bIsReady,iter->second->bIsAuthFail);
			if (iter->second->bIsAuthFail)
			{
				LOG4_ERROR("bIsAuthFail = %d!",iter->second->bIsAuthFail);//目前验证失败没有重试，也不再压入请求队列
				redisAsyncDisconnect((redisAsyncContext*)pRedisContext);/* 主动断开连接对象，会调用断开连接回调函数 */
				return(false);
			}
            LOG4_TRACE("listWaitData.push_back()");
            iter->second->listWaitData.push_back(std::unique_ptr<RedisStep>(pRedisStep));
            return(true);
        }
    }
}

bool Worker::ResetTimeout(Step* pStep, struct ev_timer* watcher)
{
    ev_tstamp after = pStep->GetTimeout();//ev_now(m_loop) + pStep->GetTimeout();
    RefreshEvent(after,watcher);
    LOG4_TRACE("%s() pStep %u after(%lf) set time(%lf) ev_now(%lf)",
    		__FUNCTION__,pStep->GetSequence(),after,after + ev_time() - ev_now(m_loop),ev_now(m_loop));
    return(true);
}

Session* Worker::GetSession(uint64 uiSessionId, const std::string& strSessionClass)
{
	auto name_iter = mapCallbackSession.find(strSessionClass);
    if (name_iter == mapCallbackSession.end())
    {
        return(nullptr);
    }
    else
    {
        char szSession[32] = {0};
        snprintf(szSession, sizeof(szSession), "%llu", uiSessionId);
        auto id_iter = name_iter->second.find(szSession);
        if (id_iter == name_iter->second.end())
        {
            LOG4_TRACE("szSession(%s).strSessionClass(%s) not exist",szSession,strSessionClass.c_str());
            return(nullptr);
        }
        else
        {
            id_iter->second->SetActiveTime(ev_now(m_loop));
            return(id_iter->second);
        }
    }
}

Session* Worker::GetSession(const std::string& strSessionId, const std::string& strSessionClass)
{
    auto name_iter = mapCallbackSession.find(strSessionClass);
    if (name_iter == mapCallbackSession.end())
    {
        return(nullptr);
    }
    else
    {
        auto id_iter = name_iter->second.find(strSessionId);
        if (id_iter == name_iter->second.end())
        {
            return(nullptr);
        }
        else
        {
            id_iter->second->SetActiveTime(ev_now(m_loop));
            return(id_iter->second);
        }
    }
}

bool Worker::Disconnect(const tagMsgShell& stMsgShell, bool bMsgShellNotice)
{
    auto iter = mapFdAttr.find(stMsgShell.iFd);
    if (iter != mapFdAttr.end())
    {
        if (iter->second->ulSeq == stMsgShell.ulSeq)
        {
            LOG4_TRACE("if (iter->second->ulSeq == stMsgShell.ulSeq)");
            return(DestroyConnect(iter, bMsgShellNotice));
        }
    }
    return(false);
}

bool Worker::Disconnect(const std::string& strIdentify, bool bMsgShellNotice)
{
    auto shell_iter = mapMsgShell.find(strIdentify);
    if (shell_iter == mapMsgShell.end())
    {
        return(true);
    }
    else
    {
        return(Disconnect(shell_iter->second, bMsgShellNotice));
    }
}

void Worker::SetProcessName(const util::CJsonObject& oJsonConf)
{
	std::string strServerName = std::move(oJsonConf("server_name"));
	if (strServerName.size() > 0 && m_strServerName != strServerName)
	{
		char szProcessName[64] = {0};
		snprintf(szProcessName, sizeof(szProcessName), "%s_W%d", strServerName.c_str(), iWorkerIndex);
		ngx_setproctitle(szProcessName);
		m_strServerName = strServerName;
	}
}

bool Worker::Init(util::CJsonObject& oJsonConf)
{
	nPid = getpid();
	SetProcessName(oJsonConf);

    oJsonConf.Get("io_timeout", m_dIoTimeout);
    oJsonConf.Get("step_timeout", m_dStepTimeout);

    bool bCheckInternalRouter = true;
    uint32 uiHeartbeatTime = 20;
    oJsonConf.Get("check_internal_router", bCheckInternalRouter);
    oJsonConf.Get("check_internal_heartbeattime", uiHeartbeatTime);
    nodesMgr.SetCheckInternalRouter(bCheckInternalRouter,uiHeartbeatTime);

    oJsonConf.Get("access_verify_time", m_iAccessVerifyTime);
    oJsonConf.Get("node_type", m_strNodeType);
    oJsonConf.Get("inner_host", m_strHostForServer);

    if (m_strHostForServer.size() == 0)
	{
		if (GetHostForServer().size() == 0)
		{
			std::cerr << "DomainToIP "<< m_pErrBuff <<" error!" << std::endl;
			return false;
		}
	}

    oJsonConf.Get("inner_port", m_iPortForServer);
    int32 iCodec = util::CODEC_PB_INTERNAL;
    if (oJsonConf.Get("access_codec", iCodec))
    {
        m_eAccessCodec = util::E_CODEC_TYPE(iCodec);
    }
    m_bWorkerReuseportAccept = true;
    if (m_bWorkerReuseportAccept)
    {
        if (oJsonConf.Get("access_host", m_strHostForClient) && m_strHostForClient.size() == 0)
        {
            if (GetHostForClient().size() == 0)
            {
                std::cerr << "DomainToIP "<< m_pErrBuff <<" error!" << std::endl;
                return false;
            }
        }
        oJsonConf.Get("access_port", m_iPortForClient);
        oJsonConf.Get("client_socket_backlog", m_iClientSocketBackLog);
    }

    oJsonConf["permission"]["uin_permit"].Get("stat_interval", m_dMsgStatInterval);//只有Gate类型的才有的配置
    oJsonConf["permission"]["uin_permit"].Get("permit_num", m_iMsgPermitNum);

    m_oLastConf = m_oCurrentConf = oJsonConf;
    m_oCustomConf = oJsonConf["custom"];
    m_oCustomConf.Get("cat_log_system", m_bCatLogSystem);

    {
        int iPoolThreads = 4;
        (void)m_oCustomConf.Get("worker_thread_pool_size", iPoolThreads);
        if (iPoolThreads < 1)
        {
            iPoolThreads = 1;
        }
        if (iPoolThreads > static_cast<int>(THREADPOOL_MAX_NUM))
        {
            iPoolThreads = THREADPOOL_MAX_NUM;
        }
        InitThunderWorkerThreadPool(static_cast<unsigned short>(iPoolThreads));
    }

    InitLogger(oJsonConf);
    InitDataLogger(oJsonConf);
    LOG4_INFO("%s program begin, and work path %s. pid(%d)", m_strServerName.c_str(), m_strWorkPath.c_str(),nPid);
    LOG4_INFO("NetWorker build marker: DelEvent-before-close, stack_http_parser, http_init_log_trim, fix-coro-self-destroy (%s %s)",
              __DATE__, __TIME__);
    LOG4_INFO("%s() pid(%d) listen on iPortForServer(%d) strHostForServer(%s) iServerSocketBackLog(%d)",
        		__FUNCTION__,nPid,m_iPortForServer,m_strHostForServer.c_str(),m_iServerSocketBackLog);

    mapCodec.insert(std::make_pair(util::CODEC_PB_INTERNAL, std::make_unique<ProtoCodec>(util::CODEC_PB_INTERNAL)));
    mapCodec.insert(std::make_pair(util::CODEC_HTTP, std::make_unique<HttpCodec>(util::CODEC_HTTP)));
    mapCodec.insert(std::make_pair(util::CODEC_HTTPS, std::make_unique<HttpsCodec>()));
    mapCodec.insert(std::make_pair(util::CODEC_PRIVATE, std::make_unique<ClientMsgCodec>(util::CODEC_PRIVATE)));
    mapCodec.insert(std::make_pair(util::CODEC_WEBSOCKET_EX_JS, std::make_unique<CodecWebSocketJson>(util::CODEC_WEBSOCKET_EX_JS)));
    mapCodec.insert(std::make_pair(util::CODEC_WEBSOCKET_EX_PB, std::make_unique<CodecWebSocketPb>(util::CODEC_WEBSOCKET_EX_PB)));
	mapCodec.insert(std::make_pair(util::CODEC_TEST, std::make_unique<CodecCustom>(util::CODEC_TEST)));
	mapCodec.insert(std::make_pair(util::CODEC_APP, std::make_unique<AppMsgCodec>(util::CODEC_APP)));
	mapCodec.insert(std::make_pair(util::CODEC_WEBSOCKET_EX_PB_APP, std::make_unique<CodecWebSocketPbApp>(util::CODEC_WEBSOCKET_EX_PB_APP)));
    {
        auto codec_iter = mapCodec.find(util::CODEC_HTTPS);
        if (codec_iter != mapCodec.end())
        {
            HttpsCodec::HttpsConfig oHttpsCfg;
            util::CJsonObject oHttpsConf = m_oCustomConf["https"];
            util::CJsonObject oHttpsServer = oHttpsConf["server"];
            util::CJsonObject oHttpsClient = oHttpsConf["client"];
            oHttpsServer.Get("cert_file", oHttpsCfg.strServerCertFile);
            oHttpsServer.Get("key_file", oHttpsCfg.strServerKeyFile);
            oHttpsServer.Get("ca_file", oHttpsCfg.strServerCaFile);
            oHttpsServer.Get("verify_client", oHttpsCfg.bServerVerifyClient);
            oHttpsClient.Get("ca_file", oHttpsCfg.strClientCaFile);
            oHttpsClient.Get("verify_peer", oHttpsCfg.bClientVerifyPeer);
            static_cast<HttpsCodec*>(codec_iter->second.get())->SetHttpsConfig(oHttpsCfg);
        }
    }

    bool bCpuAffinity = false;
	oJsonConf.Get("cpu_affinity", bCpuAffinity);
	if (bCpuAffinity)
	{
		/* get logical cpu number */
		int iCpuNum = sysconf(_SC_NPROCESSORS_CONF);                        ///< cpu数量
		cpu_set_t stCpuMask;                                                        ///< cpu set
		CPU_ZERO(&stCpuMask);
		CPU_SET(iWorkerIndex % iCpuNum, &stCpuMask);
		LOG4_INFO("sched_setaffinity iCpuNum(%d) iWorkerIndex(%d).",iCpuNum,iWorkerIndex);
		if (sched_setaffinity(0, sizeof(cpu_set_t), &stCpuMask) == -1)
		{
			LOG4_WARN("sched_setaffinity failed.");
		}
	}
    return(true);
}

bool Worker::CreateEvents()
{
    m_loop = ev_loop_new(EVBACKEND_EPOLL);
    if (m_loop == nullptr)
    {
        return(false);
    }

    signal(SIGPIPE, SIG_IGN);
    // 注册信号事件
    AddSignal(SIGINT,TerminatedCallback);
	AddSignal(SIGKILL,TerminatedCallback);
	AddSignal(SIGTERM,TerminatedCallback);

    AddPeriodicTaskEvent();

    // 注册网络IO事件
    if (CreateManagerFdAttr(iManagerControlFd,GetFdSequence()) == nullptr)
    {
    	LOG4_WARN("CreateManagerFdAttr(iManagerControlFd) == nullptr");
    	return(false);
    }
    if (CreateManagerFdAttr(iManagerDataFd,GetFdSequence()) == nullptr)
	{
		LOG4_WARN("CreateManagerFdAttr(iManagerDataFd) == nullptr");
		return(false);
	}
    if (m_bWorkerReuseportAccept && !InitClientListener())
    {
        LOG4_ERROR("InitClientListener() failed in worker_reuseport mode.");
        return false;
    }
    InitPostToEventLoop();
    return(true);
}

bool Worker::InitClientListener()
{
    if (!IsAccess())
    {
        LOG4_WARN("%s() worker_reuseport enabled but access host/port invalid.", __FUNCTION__);
        return true;
    }
    int iFd = -1;
    struct sockaddr addr;
    if (!HostPort2SockAddr(m_strHostForClient, m_iPortForClient, addr, iFd, true))
    {
        LOG4_ERROR("HostPort2SockAddr error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        return false;
    }
#ifdef SO_REUSEPORT
    int iOpt = 1;
    if (setsockopt(iFd, SOL_SOCKET, SO_REUSEPORT, &iOpt, sizeof(iOpt)) != 0)
    {
        LOG4_ERROR("setsockopt SO_REUSEPORT error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        close(iFd);
        return false;
    }
#else
    LOG4_ERROR("SO_REUSEPORT not supported by current build environment.");
    close(iFd);
    return false;
#endif
    if (bind(iFd, &addr, sizeof(addr)) < 0)
    {
        LOG4_ERROR("bind error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        close(iFd);
        return false;
    }
    if (listen(iFd, m_iClientSocketBackLog) < 0)
    {
        LOG4_ERROR("listen error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        close(iFd);
        return false;
    }
    m_iC2SListenFd = iFd;
    tagConnectionAttr* pListenConn = CreateFdAttr(m_iC2SListenFd, GetFdSequence(), util::CODEC_PB_INTERNAL);
    if (pListenConn == nullptr)
    {
        LOG4_ERROR("CreateFdAttr failed for worker listen fd %d", m_iC2SListenFd);
        CloseSocket(m_iC2SListenFd);
        return false;
    }
    if (!AddIoReadEvent(pListenConn))
    {
        LOG4_ERROR("AddIoReadEvent failed for worker listen fd %d", m_iC2SListenFd);
        DestroyConnect(mapFdAttr.find(m_iC2SListenFd));
        return false;
    }
    LOG4_INFO("%s() worker_%d listen on iPortForClient(%d) strHostForClient(%s) iClientSocketBackLog(%d)",
              __FUNCTION__, iWorkerIndex, m_iPortForClient, m_strHostForClient.c_str(), m_iClientSocketBackLog);
    return true;
}

void Worker::AddCmd(Cmd* pCmd,int iCmd)
{
	pCmd->SetCmd(iCmd);
	mapSysCmd.insert(std::make_pair(iCmd, std::unique_ptr<Cmd>(pCmd)));
}

void Worker::PreloadCmd()
{
	AddCmd(new CmdBeat(),CMD_REQ_BEAT);
	AddCmd(new CmdHeartBeatRes(),CMD_RSP_BEAT);
    AddCmd(new CmdToldWorker(),CMD_REQ_TELL_WORKER);
    AddCmd(new CmdUpdateNodeId(),CMD_REQ_REFRESH_NODE_ID);
    AddCmd(new CmdNodeNotice(),CMD_REQ_NODE_REG_NOTICE);

    AddCmd(new CmdUpdateConfig(),CMD_REQ_SERVER_CONFIG);
    AddCmd(new CmdSetLogLevel(),CMD_REQ_SET_LOG_LEVEL);
    AddCmd(new CmdLogicConfig(),CMD_REQ_RELOAD_LOGIC_CONFIG);

    AddCmd(new CmdReloadSo(),CMD_REQ_RELOAD_SO);
    AddCmd(new CmdReloadModule(),CMD_REQ_RELOAD_MODULE);
    AddCmd(new CmdReloadCustom(),CMD_REQ_SET_NODE_CUSTOM_CONFIG);
}

void Worker::RemoveFdAttrForShutdown(std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>>::iterator iter)
{
    if (iter == mapFdAttr.end())
    {
        return;
    }
    tagConnectionAttr* pConn = iter->second.get();
    tagMsgShell stMsgShell(pConn->iFd, pConn->ulSeq);
    DelInnerFd(stMsgShell);
    auto http_step_iter = mapHttpAttr.find(stMsgShell.iFd);
    if (http_step_iter != mapHttpAttr.end())
    {
        mapHttpAttr.erase(http_step_iter);
    }
    DelMsgShell(pConn->strIdentify, stMsgShell);
    if (pConn->pIoWatcher != nullptr)
    {
        DelEvent(pConn->pIoWatcher, (tagIoWatcherData*)pConn->pIoWatcher->data);
    }
    if (pConn->pTimeWatcher != nullptr)
    {
        DelEvent(pConn->pTimeWatcher, (tagIoWatcherData*)pConn->pTimeWatcher->data);
    }
    (void)::close(pConn->iFd);
    mapFdAttr.erase(iter);
}

void Worker::Destroy()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    mapHttpAttr.clear();
    mapSysCmd.clear();

    mapSo.clear();

    mapModule.clear();

    while (!mapFdAttr.empty())
    {
        auto it = mapFdAttr.begin();
        tagConnectionAttr* pConn = it->second.get();
        if (pConn->iFd == iManagerControlFd || pConn->iFd == iManagerDataFd)
        {
            LOG4_TRACE("%s() shutdown teardown manager ipc fd %d", __FUNCTION__, pConn->iFd);
            RemoveFdAttrForShutdown(it);
            continue;
        }
        const int iFdKey = it->first;
        if (!DestroyConnect(it))
        {
            auto again = mapFdAttr.find(iFdKey);
            if (again != mapFdAttr.end())
            {
                LOG4_WARN("%s() DestroyConnect left fd %d in map; forced shutdown teardown", __FUNCTION__, iFdKey);
                RemoveFdAttrForShutdown(again);
            }
        }
    }

    mapCodec.clear();

    if (m_loop != nullptr)
    {
        StopPostToEventLoop();
        ev_loop_destroy(m_loop);
        m_loop = nullptr;
    }
}

bool Worker::AddMsgShell(const std::string& strIdentify, const tagMsgShell& stMsgShell)
{
    LOG4_TRACE("%s(%s, fd %d, seq %u)", __FUNCTION__, strIdentify.c_str(), stMsgShell.iFd, stMsgShell.ulSeq);
    auto shell_iter = mapMsgShell.find(strIdentify);
    if (shell_iter == mapMsgShell.end())
    {
        mapMsgShell.insert(std::make_pair(strIdentify, stMsgShell));
    }
    else
    {
        if ((stMsgShell.iFd != shell_iter->second.iFd || stMsgShell.ulSeq != shell_iter->second.ulSeq))
        {
            LOG4_TRACE("%s() connect to %s was exist, replace old fd %d with new fd %d",__FUNCTION__,strIdentify.c_str(), shell_iter->second.iFd, stMsgShell.iFd);
            auto fd_iter = mapFdAttr.find(shell_iter->second.iFd);
            if (GetWorkerIdentify() != strIdentify)//外部连接
            {
                LOG4_TRACE("%s() GetWorkerIdentify() != strIdentify. replace old stMsgShell(%d,%u) with new stMsgShell(%d,%u)",
                                __FUNCTION__,shell_iter->second.iFd,shell_iter->second.ulSeq,stMsgShell.iFd,stMsgShell.ulSeq);
                shell_iter->second = stMsgShell;
            }
            else
            {
                LOG4_TRACE("%s() replace old stMsgShell(%d,%u) with new stMsgShell(%d,%u)",
                                __FUNCTION__,shell_iter->second.iFd,shell_iter->second.ulSeq,stMsgShell.iFd,stMsgShell.ulSeq);
                shell_iter->second = stMsgShell;
            }
        }
    }
    SetConnectIdentify(stMsgShell, strIdentify);
    return(true);
}

void Worker::DelMsgShell(const std::string& strIdentify,const tagMsgShell& stMsgShell)
{
    auto shell_iter = mapMsgShell.find(strIdentify);
    if (shell_iter == mapMsgShell.end())
    {
        LOG4_TRACE("%s() strIdentify(%s) don't has stMsgShell",__FUNCTION__,strIdentify.c_str());
    }
    else
    {
        if(stMsgShell.iFd && stMsgShell.ulSeq)
        {
            if(stMsgShell.iFd == shell_iter->second.iFd && stMsgShell.ulSeq == shell_iter->second.ulSeq)
            {
                LOG4_TRACE("%s() strIdentify(%s) del map stMsgShell(%d,%u)",__FUNCTION__,strIdentify.c_str(),shell_iter->second.iFd,shell_iter->second.ulSeq);
                mapMsgShell.erase(shell_iter);
            }
            else
            {
                LOG4_TRACE("%s() strIdentify(%s) del stMsgShell(%d,%u) and map stMsgShell(%d,%u) are not the same",
                                __FUNCTION__,strIdentify.c_str(),stMsgShell.iFd,stMsgShell.ulSeq,shell_iter->second.iFd,shell_iter->second.ulSeq);
            }
        }
        else
        {
            LOG4_TRACE("%s() strIdentify(%s) del map stMsgShell(%d,%u)",
                            __FUNCTION__,strIdentify.c_str(),shell_iter->second.iFd,shell_iter->second.ulSeq);
            mapMsgShell.erase(shell_iter);
        }
    }
// 连接虽然断开，但不应清除节点标识符，这样可以保证下次有数据发送时可以重新建立连接
//    std::unordered_map<std::string, std::string>::iterator identify_iter = m_mapIdentifyNodeType.find(strIdentify);
//    if (identify_iter != m_mapIdentifyNodeType.end())
//    {
//        std::unordered_map<std::string, std::pair<std::set<std::string>::iterator, std::set<std::string> > >::iterator node_type_iter;
//        node_type_iter = m_mapNodeIdentify.find(identify_iter->second);
//        if (node_type_iter != m_mapNodeIdentify.end())
//        {
//            std::set<std::string>::iterator id_iter = node_type_iter->second.second.find(strIdentify);
//            if (id_iter != node_type_iter->second.second.end())
//            {
//                node_type_iter->second.second.erase(id_iter);
//                node_type_iter->second.first = node_type_iter->second.second.begin();
//            }
//        }
//        m_mapIdentifyNodeType.erase(identify_iter);
//    }
}


void Worker::AddNodeIdentify(const std::string& strNodeType, const std::string& strIdentify)
{
    LOG4_TRACE("%s(%s, %s)", __FUNCTION__, strNodeType.c_str(), strIdentify.c_str());
    nodesMgr.AddNodeIdentify(strNodeType,strIdentify);
}

void Worker::DelNodeIdentify(const std::string& strNodeType, const std::string& strIdentify)
{
    LOG4_TRACE("%s(%s, %s)", __FUNCTION__, strNodeType.c_str(), strIdentify.c_str());
    nodesMgr.DelNodeIdentify(strNodeType,strIdentify);
}

void Worker::GetNodeIdentifys(const std::string& strNodeType, std::vector<std::string>& strIdentifys)
{
    strIdentifys.clear();
    const auto& identifySet = nodesMgr.GetNodeIdentifys(strNodeType);
    for(const auto & identify_iter:identifySet)
    {
        strIdentifys.push_back(identify_iter);
    }
}

bool Worker::HasNodeIdentifys(const std::string& strNodeType)
{
	const auto& identifySet = nodesMgr.GetNodeIdentifys(strNodeType);
	return (identifySet.size() > 0);
}

bool Worker::RegisterCallback(const std::string& strIdentify, RedisStep* pRedisStep)
{
    LOG4_TRACE("%s(%s)", __FUNCTION__, strIdentify.c_str());
    int iPosIpPortSeparator = strIdentify.find(',');
    if (iPosIpPortSeparator != std::string::npos)
    {
        return(AutoRedisCluster(strIdentify,pRedisStep));
    }
    iPosIpPortSeparator = strIdentify.find(':');//"192.168.11.71:29000@abcab"
    if (iPosIpPortSeparator == std::string::npos)
    {
    	LOG4_ERROR("strIdentify error: %s", strIdentify.c_str());
        return(false);
    }
    int iPassWordSeparator = strIdentify.find('@');
    std::string strHost = strIdentify.substr(0, iPosIpPortSeparator);
    std::string strPort;
    std::string strPassword;
    if (iPassWordSeparator == std::string::npos)
    {
    	strPort = strIdentify.substr(iPosIpPortSeparator + 1, std::string::npos);
    }
    else
    {
    	strPort = strIdentify.substr(iPosIpPortSeparator + 1, iPassWordSeparator);
    	strPassword = strIdentify.substr(iPassWordSeparator + 1, std::string::npos);
    }
    int iPort = atoi(strPort.c_str());
    if (iPort == 0)
    {
    	LOG4_ERROR("strIdentify error: %s", strIdentify.c_str());
        return(false);
    }
    char szIdentify[32] = {0};
	snprintf(szIdentify, 32, "%s:%d", strHost.c_str(), iPort);
    auto ctx_iter = mapRedisContext.find(szIdentify);
    if (ctx_iter != mapRedisContext.end())
    {
        LOG4_TRACE("redis context %s", szIdentify);
        return(RegisterCallback(ctx_iter->second, pRedisStep));
    }
    else
    {
        LOG4_TRACE("AutoRedisCmd(%s, %d, %s)", strHost.c_str(), iPort,strPassword.c_str());
        return(AutoRedisCmd(strHost, iPort, pRedisStep,strPassword));
    }
}

bool Worker::RegisterCallback(const std::string& strHost, int iPort, RedisStep* pRedisStep)
{
    LOG4_TRACE("%s(%s, %d)", __FUNCTION__, strHost.c_str(), iPort);
    char szIdentify[32] = {0};
    snprintf(szIdentify, sizeof(szIdentify), "%s:%d", strHost.c_str(), iPort);
    auto ctx_iter = mapRedisContext.find(szIdentify);
    if (ctx_iter != mapRedisContext.end())
    {
        LOG4_TRACE("redis context %s", szIdentify);
        return(RegisterCallback(ctx_iter->second, pRedisStep));
    }
    else
    {
        LOG4_TRACE("GetLabor()->AutoRedisCmd(%s, %d)", strHost.c_str(), iPort);
        return(AutoRedisCmd(strHost, iPort, pRedisStep));
    }
}

bool Worker::AddRedisContextAddr(const std::string& strHost, int iPort, redisAsyncContext* ctx)
{
    LOG4_TRACE("%s(%s, %d, 0x%p)", __FUNCTION__, strHost.c_str(), iPort, ctx);
    char szIdentify[32] = {0};
    snprintf(szIdentify, 32, "%s:%d", strHost.c_str(), iPort);
    return WorkerRuntimeContext::AddRedisContextAddr(szIdentify, ctx);
}

void Worker::DelRedisContextAddr(const redisAsyncContext* ctx)
{
    std::string identify;
    if (WorkerRuntimeContext::DelRedisContextAddr(ctx, &identify))
    {
        LOG4_TRACE("%s(%s,0x%p)", __FUNCTION__, identify.c_str(), ctx);
    }
}

bool Worker::RegisterCallback(MysqlStep* pMysqlStep)
{
#define MYSQL_CONTEXT_MAP_MAX_SIZE (10)
	LOG4_TRACE("%s pMysqlStep(%s, %d)", __FUNCTION__, pMysqlStep->m_strHost.c_str(), pMysqlStep->m_iPort);
	if (pMysqlStep->CurTask().size() == 0 || pMysqlStep->m_strHost.size() == 0 || 0 == pMysqlStep->m_iPort)//MysqlStep必须含mysql访问任务
    {
        LOG4_ERROR("%s() CurTask(%zu) strHost(%zu) iPort(%d)",__FUNCTION__,pMysqlStep->CurTask().size(),pMysqlStep->m_strHost.size(),pMysqlStep->m_iPort);
        return(false);
    }
	char szIdentify[64] = {0};
	snprintf(szIdentify, sizeof(szIdentify), "%s:%d:%s", pMysqlStep->m_strHost.c_str(),pMysqlStep->m_iPort,pMysqlStep->m_strDbName.c_str());
	auto ctx_iter = mapMysqlContext.find(szIdentify);
	if (ctx_iter != mapMysqlContext.end())//每个连接符对应最多10连接
	{
		if (ctx_iter->second.second.size() < MYSQL_CONTEXT_MAP_MAX_SIZE)
		{//new conn
			return(AutoMysqlCmd(pMysqlStep));
		}
		util::MysqlAsyncConn* pMysqlConn(nullptr);
		if (ctx_iter->second.first == ctx_iter->second.second.end())
		{
			ctx_iter->second.first = ctx_iter->second.second.begin();
		}
		pMysqlConn = *ctx_iter->second.first;
		++ctx_iter->second.first;
		LOG4_TRACE("mysql conn %s", szIdentify);
		return(RegisterCallback(pMysqlConn, pMysqlStep));
	}
	else
	{//new conn
		LOG4_TRACE("AutoMysqlCmd(%s, %d)", pMysqlStep->m_strHost.c_str(), pMysqlStep->m_iPort);
		return(AutoMysqlCmd(pMysqlStep));
	}
}

bool Worker::RegisterCallback(util::MysqlAsyncConn* pMysqlContext, MysqlStep* pMysqlStep)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (pMysqlStep == nullptr)
    {
        return(false);
    }
    static uint64 uiAddTaskCounter = 0;
	++uiAddTaskCounter;
    if (!pMysqlStep->IsRegistered())  // 已注册过，不必重复注册，不过认为本次注册成功,任务会继续投递
    {
		pMysqlStep->SetRegistered();
		LOG4_TRACE("register pMysqlStep ok");
    }
    if (pMysqlStep->m_strCmd.size() > 0)
    {
    	LOG4_TRACE("succeed in add mysql cmd: %s:%u uiAddTaskCounter:%llu",pMysqlStep->m_strCmd.c_str(),pMysqlStep->m_uiCmdType,uiAddTaskCounter);
    	util::SqlTask *task = new util::SqlTask(pMysqlStep->m_strCmd,(util::eSqlTaskOper)pMysqlStep->m_uiCmdType,new CustomMysqlHandler(pMysqlStep));
		pMysqlContext->add_task(task);
		pMysqlStep->ClearTask();
    }
	return(true);
}

bool Worker::AddMysqlContextAddr(MysqlStep* pMysqlStep, util::MysqlAsyncConn* ctx)
{
    LOG4_TRACE("%s(%s, %d, 0x%p)", __FUNCTION__, pMysqlStep->m_strHost.c_str(), pMysqlStep->m_iPort, ctx);
    char szIdentify[64] = {0};
	snprintf(szIdentify, sizeof(szIdentify), "%s:%d:%s", pMysqlStep->m_strHost.c_str(),pMysqlStep->m_iPort,pMysqlStep->m_strDbName.c_str());
    auto ctx_iter = mapMysqlContext.find(szIdentify);
    if (ctx_iter == mapMysqlContext.end())
    {
    	std::set<util::MysqlAsyncConn*> set;
    	set.insert(ctx);
    	mapMysqlContext.insert(std::pair<std::string, std::pair<std::set<util::MysqlAsyncConn*>::iterator,std::set<util::MysqlAsyncConn*> > >
    	(szIdentify,std::make_pair(set.begin(),set)));
        std::unordered_map<util::MysqlAsyncConn*, std::string>::iterator identify_iter = mapMysqlContextIdentify.find(ctx);
        if (identify_iter == mapMysqlContextIdentify.end())
        {
        	mapMysqlContextIdentify.insert(std::make_pair(ctx, szIdentify));
        }
        else
        {
            identify_iter->second = szIdentify;
        }
        return(true);
    }
    else
    {
    	ctx_iter->second.second.insert(ctx);
    	ctx_iter->second.first = ctx_iter->second.second.begin();
        return(false);
    }
}

void Worker::DelMysqlContextAddr(util::MysqlAsyncConn* ctx)
{
	auto identify_iter = mapMysqlContextIdentify.find(ctx);
    if (identify_iter != mapMysqlContextIdentify.end())
    {
    	auto ctx_iter = mapMysqlContext.find(identify_iter->second);
        if (ctx_iter != mapMysqlContext.end())
        {
        	ctx_iter->second.second.erase(ctx);
        }
        mapMysqlContextIdentify.erase(identify_iter);
    }
}

bool Worker::SendTo(const tagMsgShell& stMsgShell)
{
    LOG4_TRACE("%s(fd %d, seq %u) pWaitForSendBuff", __FUNCTION__, stMsgShell.iFd, stMsgShell.ulSeq);
    auto iter = mapFdAttr.find(stMsgShell.iFd);
    if (iter == mapFdAttr.end())
    {
        LOG4_ERROR("no fd %d found in mapFdAttr", stMsgShell.iFd);
        return(false);
    }
    else
    {
    	tagConnectionAttr* pConn = iter->second.get();
        if (pConn->ulSeq == stMsgShell.ulSeq && pConn->iFd == stMsgShell.iFd)
        {
            int iErrno = 0;
            int iWriteLen = 0;
            int iNeedWriteLen = (int)(pConn->pWaitForSendBuff->ReadableBytes());
            int iWriteIdx = pConn->pSendBuff->GetWriteIndex();
            iWriteLen = pConn->pSendBuff->Write(pConn->pWaitForSendBuff.get(), pConn->pWaitForSendBuff->ReadableBytes());
            if (iWriteLen == iNeedWriteLen)
            {
                iNeedWriteLen = (int)pConn->pSendBuff->ReadableBytes();
                iWriteLen = pConn->pSendBuff->WriteFD(stMsgShell.iFd, iErrno);
                iter->second->pSendBuff->Compact(8192);
                if (iWriteLen < 0)
                {
                    if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
                    {
                        LOG4_ERROR("send to fd %d error %d: %s",stMsgShell.iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        DestroyConnect(iter);
                        return(false);
                    }
                    else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
                    {
                        iter->second->dActiveTime = ev_now(m_loop);
                        AddIoWriteEvent(pConn);
                    }
                }
                else if (iWriteLen > 0)
                {
                    iSendByte += iWriteLen;
                    iter->second->dActiveTime = ev_now(m_loop);
                    if (iWriteLen == iNeedWriteLen)  // 已无内容可写，取消监听fd写事件
                    {
                        RemoveIoWriteEvent(pConn);
                    }
                    else    // 内容未写完，添加或保持监听fd写事件
                    {
                        AddIoWriteEvent(pConn);
                    }
                }
                return(true);
            }
            else
            {
                LOG4_ERROR("write to send buff error, iWriteLen = %d!", iWriteLen);
                iter->second->pSendBuff->SetWriteIndex(iWriteIdx);
                return(false);
            }
        }
        else
        {
        	LOG4_ERROR("pConn ulSeq(%u) iFd(%d) stMsgShell ulSeq(%u) iFd(%d) not match",pConn->ulSeq,pConn->iFd,stMsgShell.ulSeq,stMsgShell.iFd);
        }
    }
    return(false);
}

bool Worker::ParseMsgBody(const MsgBody& oInMsgBody,google::protobuf::Message &message)
{
	if (oInMsgBody.body().size() > 0)
	{
		if (message.ParseFromString(oInMsgBody.body()))
		{
			return true;
		}
		google::protobuf::util::JsonParseOptions oOption;
		const std::string jsonUtf8(oInMsgBody.body().data(), oInMsgBody.body().size());
		absl::Status oStatus = google::protobuf::util::JsonStringToMessage(jsonUtf8, &message, oOption);
		if (!oStatus.ok())
		{
			LOG4_ERROR("%s() body as pb or json failed(%s)! body(%s) descriptor(%s)",
			            __FUNCTION__, oStatus.ToString().c_str(), jsonUtf8.c_str(),
			            message.GetDescriptor()->DebugString().c_str());
			return false;
		}
		return true;
	}
	LOG4_TRACE("%s() empty msg body",__FUNCTION__);
	return true;
}

bool Worker::SendToClient(const tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const google::protobuf::Message &message,const std::string& additional,const std::string& strTargetId,bool boJsonBody)
{
    MsgBody oMsgBody;
    MsgHead oOutMsgHead;
	oOutMsgHead.set_seq(oInMsgHead.seq());
	oOutMsgHead.set_cmd(oInMsgHead.cmd()+1);
    BuildMsgBody(oOutMsgHead,oMsgBody,message,additional,strTargetId,boJsonBody);//会设置oOutMsgHead.msgbody_len
    LOG4_TRACE("%s oInMsgHead(cmd:%u),oOutMsgHead(cmd:%u msgbody_len:%u)",__FUNCTION__,oInMsgHead.cmd(),oOutMsgHead.cmd(),oOutMsgHead.msgbody_len());
    return SendTo(stMsgShell,oOutMsgHead,oMsgBody);
}

bool Worker::SendToClient(const std::string& strIdentify,const MsgHead& oInMsgHead,const google::protobuf::Message &message,const std::string& additional,const std::string& strTargetId,bool boJsonBody)
{
    LOG4_TRACE("%s(identify: %s)", __FUNCTION__, strIdentify.c_str());
    MsgBody oMsgBody;
    MsgHead oOutMsgHead;
    oOutMsgHead.set_seq(oInMsgHead.seq());
    oOutMsgHead.set_cmd(oInMsgHead.cmd()+1);
    BuildMsgBody(oOutMsgHead,oMsgBody,message,additional,strTargetId,boJsonBody);//会设置oOutMsgHead.msgbody_len
    auto shell_iter = mapMsgShell.find(strIdentify);
    if (shell_iter == mapMsgShell.end())
    {
        LOG4_TRACE("no tagMsgShell match %s.", strIdentify.c_str());
        return(AutoSend(strIdentify, oOutMsgHead, oMsgBody));
    }
    else
    {
        return(SendTo(shell_iter->second, oOutMsgHead, oMsgBody));
    }
}

bool Worker::SendToClient(const tagMsgShell& stInMsgShell,const MsgHead& oInMsgHead,const std::string &strBody)
{
    // auto conn_iter = mapFdAttr.find(stInMsgShell.iFd);
    // if (conn_iter == mapFdAttr.end() || conn_iter->second->ulSeq != stInMsgShell.ulSeq)
    // {
    //     LOG4_WARN("%s() skip response to stale shell(fd %d, seq %u)",
    //               __FUNCTION__, stInMsgShell.iFd, stInMsgShell.ulSeq);
    //     return false;
    // }

	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_seq(oInMsgHead.seq());
	oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	if (!SendTo(stInMsgShell, oOutMsgHead, oOutMsgBody))
	{
		LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", stInMsgShell.iFd, stInMsgShell.ulSeq);
		return false;
	}
	return true;
}

bool Worker::SendToClient(const tagMsgShell& stInMsgShell,const HttpMsg& oInHttpMsg,const std::string &strBody,int iCode,const std::unordered_map<std::string,std::string> &heads)
{
    // auto conn_iter = mapFdAttr.find(stInMsgShell.iFd);
    // if (conn_iter == mapFdAttr.end() || conn_iter->second->ulSeq != stInMsgShell.ulSeq)
    // {
    //     LOG4_WARN("%s() skip response to stale shell(fd %d, seq %u)",
    //               __FUNCTION__, stInMsgShell.iFd, stInMsgShell.ulSeq);
    //     return false;
    // }

	HttpMsg oHttpMsg;
	for(const auto & iter:heads)
	{
		oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>(iter.first, iter.second));
	}
	oHttpMsg.set_type(HTTP_RESPONSE);
	oHttpMsg.set_status_code(iCode);
	oHttpMsg.set_http_major(oInHttpMsg.http_major());
	oHttpMsg.set_http_minor(oInHttpMsg.http_minor());
	oHttpMsg.set_body(strBody);
	if (!SendTo(stInMsgShell, oHttpMsg))
	{
		LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", stInMsgShell.iFd, stInMsgShell.ulSeq);
		return false;
	}
	return true;
}

bool Worker::BuildMsgBody(MsgHead& oMsgHead,MsgBody &oMsgBody,const google::protobuf::Message &message,const std::string& additional,const std::string& strTargetId,bool boJsonBody)
{
    oMsgBody.Clear();
    if (boJsonBody)
    {
        std::string strJson;
        google::protobuf::util::JsonPrintOptions oOption;
        absl::Status oStatus = google::protobuf::util::MessageToJsonString(message,&strJson,oOption);
        if(!oStatus.ok())
        {
            LOG4_ERROR("MessageToJsonString failed error(%s)",oStatus.ToString().c_str());
            return false;
        }
        oMsgBody.set_body(strJson);
    }
    else
    {
    	oMsgBody.set_body(message.SerializeAsString());
    }
    if(additional.size() > 0)oMsgBody.set_additional(additional);
    if(strTargetId.size() > 0)oMsgBody.set_targetid(strTargetId);

    oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
    return true;
}

bool Worker::SendToCallback(net::Session* pSession,const DataMem::MemOperate* pMemOper,SessionCallbackMem callback,const std::string &nodeTypeOrIdentify,const std::string & strModFactor,StepParam* pStepParam,uint32 uiCmd)
{
	if (!pSession)
	{
		LOG4_ERROR("pSession empty!");
		return false;
	}
	StepNode* pStep = new StepNode(pMemOper,pStepParam);
    if (pStep == nullptr)
    {
        LOG4_ERROR("new StepNode() error!");
        return(false);
    }
    if (!RegisterCallback(pStep))
    {
        LOG4_ERROR("RegisterCallback(pStep) error!");
        SAFE_DELETE(pStep);
        return(false);
    }
    pStep->SetCallBack(callback,pSession,nodeTypeOrIdentify,strModFactor,uiCmd);
    if (net::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return(false);
    }
    return true;
}

bool Worker::SendToCallback(net::Step* pUpperStep,const DataMem::MemOperate* pMemOper,StepCallbackMem callback,const std::string &nodeType,const std::string & strModFactor,StepParam* pStepParam,uint32 uiCmd)
{
	if (!pUpperStep)
	{
		LOG4_ERROR("pUpperStep null!");
		return false;
	}
	if (!pUpperStep->IsRegistered())
	{
		if (!RegisterCallback(pUpperStep))
		{
			LOG4_ERROR("RegisterCallback(pUpperStep) error!");
			SAFE_DELETE(pUpperStep);
			return(false);
		}
		LOG4_TRACE("RegisterCallback(pUpperStep)");
	}
	else
	{
		pUpperStep->DelayDel();//已经注册的需要延迟删除
	}
	StepNode* pStep = new StepNode(pMemOper,pStepParam);
    if (pStep == nullptr)
    {
        LOG4_ERROR("new StepNode() error!");
        return(false);
    }
    if (!RegisterCallback(pStep))
    {
        LOG4_ERROR("RegisterCallback(pStep) error!");
        SAFE_DELETE(pStep);
        return(false);
    }
    pStep->SetCallBack(callback,pUpperStep,nodeType,strModFactor,uiCmd);
    if (net::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return(false);
    }
    return true;
}

bool Worker::SendToCallback(net::Session* pSession,uint32 uiCmd,const std::string &strBody,SessionCallback callback,const std::string &nodeType,const std::string & strModFactor,StepParam* pStepParam)
{
	StepNode* pStep = new StepNode(strBody,pStepParam);
    if (pStep == nullptr)
    {
        LOG4_ERROR("new StepNode() error!");
        return(false);
    }
    if (!RegisterCallback(pStep))
    {
        LOG4_ERROR("RegisterCallback(pStep) error!");
        SAFE_DELETE(pStep);
        return(false);
    }
    pStep->SetCallBack(callback,pSession,nodeType,uiCmd,strModFactor);
    if (net::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return(false);
    }
    return true;
}

bool Worker::SendToCallback(net::Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StepCallback callback,const std::string &nodeTypeOrIdentify,const std::string & strModFactor,StepParam* pStepParam)
{
	if (!pUpperStep)
	{
		LOG4_ERROR("pUpperStep null!");
		return false;
	}
	if (!pUpperStep->IsRegistered())
	{
		if (!RegisterCallback(pUpperStep))
		{
			LOG4_ERROR("RegisterCallback(pUpperStep) error!");
			SAFE_DELETE(pUpperStep);
			return(false);
		}
		LOG4_TRACE("RegisterCallback(pUpperStep)");
	}
	else
	{
		pUpperStep->DelayDel();//已经注册的需要延迟删除
	}
	StepNode* pStep = new StepNode(strBody,pStepParam);
    if (pStep == nullptr)
    {
        LOG4_ERROR("new StepNode() error!");
        return(false);
    }
    if (!RegisterCallback(pStep))
    {
        LOG4_ERROR("RegisterCallback(pStep) error!");
        SAFE_DELETE(pStep);
        return(false);
    }
    pStep->SetCallBack(callback,pUpperStep,nodeTypeOrIdentify,uiCmd,strModFactor);
    if (net::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return(false);
    }
    return true;
}

bool Worker::SendToCallback(net::Session* pSession,uint32 uiCmd,const std::string &strBody,SessionCallback callback,const tagMsgShell& stMsgShell,const std::string & strModFactor,StepParam* pStepParam)
{
	StepNode* pStep = new StepNode(strBody,pStepParam);
    if (pStep == nullptr)
    {
        LOG4_ERROR("new StepNode() error!");
        return(false);
    }
    if (!RegisterCallback(pStep))
    {
        LOG4_ERROR("RegisterCallback(pStep) error!");
        SAFE_DELETE(pStep);
        return(false);
    }
    pStep->SetCallBack(callback,pSession,stMsgShell,uiCmd,strModFactor);
    if (net::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return(false);
    }
    return true;
}

bool Worker::SendToCallback(net::Step* pUpperStep,uint32 uiCmd,const std::string &strBody,StepCallback callback,const tagMsgShell& stMsgShell,const std::string & strModFactor,StepParam* pStepParam)
{
	if (!pUpperStep)
	{
		LOG4_ERROR("pUpperStep null!");
		return false;
	}
	if (!pUpperStep->IsRegistered())
	{
		if (!RegisterCallback(pUpperStep))
		{
			LOG4_ERROR("RegisterCallback(pUpperStep) error!");
			SAFE_DELETE(pUpperStep);
			return(false);
		}
		LOG4_TRACE("RegisterCallback(pUpperStep)");
	}
	else
	{
		pUpperStep->DelayDel();//已经注册的需要延迟删除
	}
	StepNode* pStep = new StepNode(strBody,pStepParam);
    if (pStep == nullptr)
    {
        LOG4_ERROR("new StepNode() error!");
        return(false);
    }
    if (!RegisterCallback(pStep))
    {
        LOG4_ERROR("RegisterCallback(pStep) error!");
        SAFE_DELETE(pStep);
        return(false);
    }
    pStep->SetCallBack(callback,pUpperStep,stMsgShell,uiCmd,strModFactor);
    if (net::STATUS_CMD_RUNNING != pStep->Emit(ERR_OK))
    {
        DeleteCallback(pStep);
        return(false);
    }
    return true;
}

bool Worker::SendToCallback(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const DataMem::MemOperate* pMemOper,StepCallbackMem callback,const std::string &nodeType ,const std::string & strModFactor,StepParam* pStepParam,uint32 uiCmd)
{
	return SendToCallback(new DataStep(stMsgShell,oInHttpMsg),pMemOper,callback,nodeType,strModFactor,pStepParam,uiCmd);
}
 
bool Worker::SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(fd %d, fd_seq %u, cmd %u, msg_seq %u)",
                    __FUNCTION__, stMsgShell.iFd, stMsgShell.ulSeq, oMsgHead.cmd(), oMsgHead.seq());
    auto conn_iter = mapFdAttr.find(stMsgShell.iFd);
    if (conn_iter == mapFdAttr.end())
    {
        LOG4_ERROR("no fd %d found in mapFdAttr", stMsgShell.iFd);
        return(false);
    }
    else
    {
    	tagConnectionAttr* pConn = conn_iter->second.get();
        if (pConn->ulSeq == stMsgShell.ulSeq && pConn->iFd == stMsgShell.iFd)
        {
            auto codec_iter = mapCodec.find(conn_iter->second->eCodecType);
            if (codec_iter == mapCodec.end())
            {
                LOG4_ERROR("no codec found for %d!", conn_iter->second->eCodecType);
                DestroyConnect(conn_iter);
                return(false);
            }
            E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
            if (util::CODEC_PB_INTERNAL == pConn->eCodecType)//内部协议需要检查连接过程
            {
                LOG4_TRACE("connect status %u", pConn->ucConnectStatus);
                if (eConnectStatus_ok != conn_iter->second->ucConnectStatus)   // 连接尚未完成
                {
                    if (oMsgHead.cmd() <= CMD_RSP_TELL_WORKER)   // 创建连接的过程
                    {
                        LOG4_TRACE("codec_iter->second->Encode,oMsgHead.cmd(%u),connect status %u",oMsgHead.cmd(),conn_iter->second->ucConnectStatus);
                        eCodecStatus = EncodeByConnectionCodec(pConn, codec_iter->second.get(), oMsgHead, oMsgBody, conn_iter->second->pSendBuff.get());
                        if(oMsgHead.cmd() == CMD_RSP_TELL_WORKER)
                        {
                            conn_iter->second->ucConnectStatus = eConnectStatus_ok;
                        }
                        else
                        {
                            conn_iter->second->ucConnectStatus = eConnectStatus_connecting;
                        }
                    }
                    else    // 创建连接过程中的其他数据发送请求
                    {
                        LOG4_TRACE("codec_iter->second->Encode,oMsgHead.cmd(%u),connect status %u",oMsgHead.cmd(),conn_iter->second->ucConnectStatus);
                        eCodecStatus = EncodeByConnectionCodec(pConn, codec_iter->second.get(), oMsgHead, oMsgBody, conn_iter->second->pWaitForSendBuff.get());
                        if (CODEC_STATUS_OK == eCodecStatus)//其他请求在连接过程中先不发送
                        {
                            return(true);
                        }
                        return(false);
                    }
                }
                else//连接已完成
                {
                    LOG4_TRACE("codec_iter->second->Encode,oMsgHead.cmd(%u),connect status %u",oMsgHead.cmd(),conn_iter->second->ucConnectStatus);
                    eCodecStatus = EncodeByConnectionCodec(pConn, codec_iter->second.get(), oMsgHead, oMsgBody, conn_iter->second->pSendBuff.get());
                }
            }
            else
            {
                LOG4_TRACE("codec_iter->second->Encode,oMsgHead.cmd(%u),connect status %u",oMsgHead.cmd(),conn_iter->second->ucConnectStatus);
                eCodecStatus = EncodeByConnectionCodec(pConn, codec_iter->second.get(), oMsgHead, oMsgBody, conn_iter->second->pSendBuff.get());
            }
            if (CODEC_STATUS_OK == eCodecStatus)
            {
                ++iSendNum;
                int iErrno = 0;
                int iNeedWriteLen = (int)conn_iter->second->pSendBuff->ReadableBytes();
                LOG4_TRACE("try send cmd[%d] seq[%u] len %d to fd %d ip %s identify %s",
					oMsgHead.cmd(), oMsgHead.seq(), iNeedWriteLen, stMsgShell.iFd,pConn->szRemoteAddr, pConn->strIdentify.c_str());
                int iWriteLen = pConn->pSendBuff->WriteFD(stMsgShell.iFd, iErrno);
                pConn->pSendBuff->Compact(8192);
                if (iWriteLen < 0)
                {
                    if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
                    {
                        LOG4_ERROR("send to fd %d error %d: %s",stMsgShell.iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        DestroyConnect(conn_iter);
                        return(false);
                    }
                    else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
                    {
                        LOG4_TRACE("write len %d, errno %d: %s",iWriteLen, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        pConn->dActiveTime = ev_now(m_loop);
                        AddIoWriteEvent(pConn);
                    }
                    else
                    {
                        LOG4_TRACE("write len %d, errno %d: %s",iWriteLen, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        pConn->dActiveTime = ev_now(m_loop);
                        AddIoWriteEvent(pConn);
                    }
                }
                else if (iWriteLen > 0)
                {
                    iSendByte += iWriteLen;
                    conn_iter->second->dActiveTime = ev_now(m_loop);
                    if (iWriteLen == iNeedWriteLen)  // 已无内容可写，取消监听fd写事件
                    {
                        LOG4_TRACE("cmd[%d] seq[%u] to fd %d ip %s identify %s need write %d and had written len %d",
								oMsgHead.cmd(), oMsgHead.seq(), stMsgShell.iFd,conn_iter->second->szRemoteAddr, conn_iter->second->strIdentify.c_str(),
								iNeedWriteLen, iWriteLen);
                        RemoveIoWriteEvent(pConn);
                    }
                    else    // 内容未写完，添加或保持监听fd写事件
                    {
                        LOG4_TRACE("cmd[%d] seq[%u] need write %d and had written len %d",oMsgHead.cmd(), oMsgHead.seq(), iNeedWriteLen, iWriteLen);
                        AddIoWriteEvent(pConn);
                    }
                }
                return(true);
            }
            else
            {
                LOG4_WARN("codec_iter->second->Encode failed,oMsgHead.cmd(%u),connect status %u",oMsgHead.cmd(),conn_iter->second->ucConnectStatus);
                return(false);
            }
        }
        else
        {
            LOG4_ERROR("fd %d sequence %u not match the iFd %d sequence %u in mapFdAttr",
                            stMsgShell.iFd, stMsgShell.ulSeq, conn_iter->second->iFd,conn_iter->second->ulSeq);
            return(false);
        }
    }
}

bool Worker::SendTo(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(identify: %s)", __FUNCTION__, strIdentify.c_str());
    auto shell_iter = mapMsgShell.find(strIdentify);
    if (shell_iter == mapMsgShell.end())
    {
        LOG4_TRACE("no tagMsgShell match %s.", strIdentify.c_str());
        return(AutoSend(strIdentify, oMsgHead, oMsgBody));
    }
    else
    {
        return(SendTo(shell_iter->second, oMsgHead, oMsgBody));
    }
}

bool Worker::SendTo(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_cmd(cmd);
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
}

bool Worker::SendTo(const std::string& strIdentify,uint32 cmd,uint32 seq,const std::string & strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_cmd(cmd);
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendTo(strIdentify, oOutMsgHead, oOutMsgBody);
}



bool Worker::SendToSession(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	if (oMsgBody.has_targetid())
	{
		#ifdef USE_CONHASH
		return SendToConHash(strNodeType, oMsgBody.targetid(), oMsgHead, oMsgBody);
		#else
		return SendToWithMod(strNodeType, oMsgBody.targetid(), oMsgHead, oMsgBody);
		#endif
	}
	else
	{
		return SendToNext(strNodeType, oMsgHead, oMsgBody);
	}
}

bool Worker::SendToSession(const std::string& strNodeType,const std::string& strTargetId,uint32 cmd,uint32 seq,const std::string & strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgBody.set_targetid(strTargetId);
	oOutMsgHead.set_cmd(cmd);
	oOutMsgHead.set_seq(seq);     // 更换消息头的seq后转发
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendToSession(strNodeType, oOutMsgHead, oOutMsgBody);
}

bool Worker::SendToClientSession(const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	if (oMsgBody.has_node_identify())
	{
		return SendTo(oMsgBody.node_identify(),oMsgHead,oMsgBody);
	}
	else if (oMsgBody.has_targetid())
	{
		net::tagMsgShell tstMsgShell;
		GetMsgShell(oMsgBody.targetid(),tstMsgShell);
		return SendTo(tstMsgShell,oMsgHead,oMsgBody);
	}
	else
	{
		LOG4_WARN("%s(no session: %s)", __FUNCTION__, oMsgBody.DebugString().c_str());
		return false;
	}
}

bool Worker::SendToClientGate(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody,uint32 status,bool aesEnc)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgBody.set_status(status);
	if (aesEnc)
	{
		oOutMsgHead.set_cmd(cmd | net::gc_app_Aes_CmdBit); //(需要编码器处理加密,采用256位aes)
	}
	else
	{
		oOutMsgHead.set_cmd(cmd);
	}
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
}

bool Worker::SendToClientGate(uint32 cmd,uint32 seq,const MsgBody& oMsgBody,uint32 status,bool aesEnc)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(oMsgBody.body());
	oOutMsgBody.set_status(status);
	net::tagMsgShell stMsgShell;
	if (oMsgBody.has_targetid())//网关发送的需要有targetid
	{
		if (!GetMsgShell(oMsgBody.targetid(),stMsgShell))
		{
			LOG4_WARN("%s() GetMsgShell error,targetid(%s)",__FUNCTION__,oMsgBody.targetid().c_str());
			return false;
		}
		{//这里是为了联调测试方便分析
			#ifdef _DEBUG
			util::CBuffer oBuff;
			if (GetClientData(stMsgShell,&oBuff))
			{
				user_basic basicinfo;//角色数据包
				if (!basicinfo.ParseFromString(oBuff.ToString()))
				{
					LOG4_ERROR("%s() cmd[%u] user_basic ParseFromString error", __FUNCTION__,cmd);
				}
				else
				{
					LOG4_TRACE("%s() cmd[%u] basicinfo(%s)", __FUNCTION__,cmd,basicinfo.DebugString().c_str());
				}
			}
			#endif
		}
	}
	else
	{
		LOG4_WARN("%s() GetMsgShell error,targetid none",__FUNCTION__);
		return false;
	}
	if (aesEnc)
	{
		oOutMsgHead.set_cmd(cmd | net::gc_app_Aes_CmdBit); //(需要编码器处理加密,采用256位aes)
	}
	else
	{
		oOutMsgHead.set_cmd(cmd);
	}
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
}

bool Worker::SendToAllClientGate(uint32 cmd,const MsgBody& oMsgBody,uint32 status,bool aesEnc)
{
	bool boRes(false);
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(oMsgBody.body());
	oOutMsgBody.set_status(status);
	for (auto iterMsgShell:mapMsgShell)
	{
		if (HadClientData(iterMsgShell.second))
		{
			if (aesEnc)
			{
				oOutMsgHead.set_cmd(cmd | net::gc_app_Aes_CmdBit); //(需要编码器处理加密,采用256位aes)
			}
			else
			{
				oOutMsgHead.set_cmd(cmd);
			}
			oOutMsgHead.set_seq(GetLabor()->GetSequence());
			oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
			if (SendTo(iterMsgShell.second, oOutMsgHead, oOutMsgBody))
			{
				boRes = true;
			}
#ifdef _DEBUG
			//这里是为了联调测试方便分析
			util::CBuffer oBuff;
			if (GetClientData(iterMsgShell.second,&oBuff))
			{
				user_basic basicinfo;//角色数据包
				if (!basicinfo.ParseFromString(oBuff.ToString()))
				{
					LOG4_ERROR("%s() cmd[%u] user_basic ParseFromString error", __FUNCTION__,cmd);
				}
				else
				{
					LOG4_TRACE("%s() cmd[%u] userid(%lu) devid(%s) sessionid(%lu)", __FUNCTION__,
							cmd,basicinfo.user_id(),basicinfo.dev_id().c_str(),basicinfo.session_id());
				}
			}
#endif
		}
	}
	return boRes;
}


bool Worker::SendToNext(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(node_type: %s)", __FUNCTION__, strNodeType.c_str());
    const auto& identify = nodesMgr.GetNodeIdentify(strNodeType);
    if (identify.size() == 0)
    {
        LOG4_ERROR("no tagMsgShell match %s!", strNodeType.c_str());
        return(false);
    }
    else
    {
    	return(SendTo(identify, oMsgHead, oMsgBody));
    }
}

bool Worker::SendToNext(const std::string& strNodeType,uint32 cmd,uint32 seq,const std::string & strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_cmd(cmd);
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendToNext(strNodeType, oOutMsgHead, oOutMsgBody);
}

bool Worker::SendToWithMod(const std::string& strNodeType, uint64 uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(nody_type: %s, mod_factor: %llu)", __FUNCTION__, strNodeType.c_str(), uiModFactor);
    const auto& identifys = nodesMgr.GetNodeIdentifys(strNodeType);
    if (identifys.size() == 0)
	{
		LOG4_ERROR("no tagMsgShell match %s!", strNodeType.c_str());
		return(false);
	}
	else
	{
		int target_identify = uiModFactor % identifys.size();
		int i = 0;
		for (const auto &id_iter :identifys)
		{
			if (i++ == target_identify)
			{
				return(SendTo(id_iter, oMsgHead, oMsgBody));
			}
		}
		return(false);
	}
}


bool Worker::SendToConHash(const std::string& strNodeType, uint64 uiModFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	const std::string& strIdentify =  nodesMgr.GetNodeIdentify(strNodeType,uiModFactor);
    LOG4_TRACE("%s(nody_type: %s, mod_factor: %llu),strIdentify:%s", __FUNCTION__, strNodeType.c_str(), uiModFactor,strIdentify.c_str());
    if (strIdentify.size() == 0)
    {
    	LOG4_ERROR("%s no strIdentify match %s!", __FUNCTION__,strIdentify.c_str());
		return(false);
    }
    return SendTo(strIdentify, oMsgHead, oMsgBody);
}

bool Worker::SendToConHash(const std::string& strNodeType, const std::string& strFactor, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	const std::string& strIdentify =  nodesMgr.GetNodeIdentify(strNodeType,strFactor);
    LOG4_TRACE("%s(nody_type: %s, strFactor: %s),strIdentify:%s", __FUNCTION__, strNodeType.c_str(), strFactor.c_str(),strIdentify.c_str());
    if (strIdentify.size() == 0)
    {
    	LOG4_ERROR("%s no strIdentify match %s!", __FUNCTION__,strIdentify.c_str());
		return(false);
    }
    return SendTo(strIdentify, oMsgHead, oMsgBody);
}


bool Worker::SendToNodeType(const std::string& strNodeType, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(node_type: %s)", __FUNCTION__, strNodeType.c_str());
    const auto& identifys = nodesMgr.GetNodeIdentifys(strNodeType);
	if (identifys.size() == 0)
	{
		LOG4_ERROR("no tagMsgShell match %s!", strNodeType.c_str());
		return(false);
	}
    else
    {
        int iSendNum = 0;
        for (const auto &id_iter : identifys)
        {
            if (id_iter != GetWorkerIdentify())
            {
                SendTo(id_iter, oMsgHead, oMsgBody);
            }
            ++iSendNum;
        }
        if (0 == iSendNum)
        {
            LOG4_ERROR("no tagMsgShell match and no node identify found for %s!", strNodeType.c_str());
            return(false);
        }
    }
    return(true);
}

bool Worker::SendTo(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg, Step* pStep)
{
    LOG4_TRACE("%s(fd %d, seq %u)", __FUNCTION__, stMsgShell.iFd, stMsgShell.ulSeq);
    auto conn_iter = mapFdAttr.find(stMsgShell.iFd);
    if (conn_iter == mapFdAttr.end())
    {
        LOG4_ERROR("no fd %d found in mapFdAttr", stMsgShell.iFd);
        return(false);
    }
    else
    {
    	tagConnectionAttr* pConn = conn_iter->second.get();
        if (pConn->ulSeq == stMsgShell.ulSeq && pConn->iFd == stMsgShell.iFd)
        {
            auto codec_iter = mapCodec.find(pConn->eCodecType);
            if (codec_iter == mapCodec.end())
            {
                LOG4_ERROR("no codec found for %d!", pConn->eCodecType);
                DestroyConnect(conn_iter);
                return(false);
            }
            E_CODEC_STATUS eCodecStatus;
            if (util::CODEC_HTTP == conn_iter->second->eCodecType
                    || util::CODEC_HTTPS == conn_iter->second->eCodecType
            		|| util::CODEC_WEBSOCKET_EX_PB == conn_iter->second->eCodecType
					|| util::CODEC_WEBSOCKET_EX_JS == conn_iter->second->eCodecType
					|| util::CODEC_WEBSOCKET_EX_PB_APP == conn_iter->second->eCodecType)
			{
				if (pConn->pWaitForSendBuff->ReadableBytes() > 0)   // 正在连接
				{
					eCodecStatus = EncodeByConnectionCodec(pConn, codec_iter->second.get(), oHttpMsg, pConn->pWaitForSendBuff.get());
					LOG4_TRACE("fd[%d], seq[%u], pWaitForSendBuff %zu", stMsgShell.iFd, stMsgShell.ulSeq, pConn->pWaitForSendBuff->ReadableBytes());
				}
				else
				{
					eCodecStatus = EncodeByConnectionCodec(pConn, codec_iter->second.get(), oHttpMsg, pConn->pSendBuff.get());
					LOG4_TRACE("fd[%d], seq[%u], pSendBuff %zu", stMsgShell.iFd, stMsgShell.ulSeq, pConn->pSendBuff->ReadableBytes());
				}
			}
            else
            {
                LOG4_ERROR("the codec for fd %d is not http or websocket codec(%d)!",stMsgShell.iFd,pConn->eCodecType);
                return(false);
            }

            if (CODEC_STATUS_OK == eCodecStatus && pConn->pSendBuff->ReadableBytes() > 0)
            {
                ++iSendNum;
                if ((pConn->pIoWatcher != nullptr) && (pConn->pIoWatcher->events & EV_WRITE))
                {   // 正在监听fd的写事件，说明发送缓冲区满，此时直接返回，等待EV_WRITE事件再执行WriteFD
                    return(true);
                }
                LOG4_TRACE("fd[%d], seq[%u], pConn->pSendBuff %p", stMsgShell.iFd, stMsgShell.ulSeq, pConn->pSendBuff.get());
                int iErrno = 0;
                int iNeedWriteLen = (int)pConn->pSendBuff->ReadableBytes();
                int iWriteLen = pConn->pSendBuff->WriteFD(stMsgShell.iFd, iErrno);
                pConn->pSendBuff->Compact(8192);
                if (iWriteLen < 0)
                {
                    if (EAGAIN != iErrno && EINTR != iErrno)  // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
                    {
                        LOG4_ERROR("send to fd %d error %d: %s",stMsgShell.iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        DestroyConnect(conn_iter);
                        return(false);
                    }
                    else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
                    {
                        LOG4_TRACE("write len %d, error %d: %s",iWriteLen, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        conn_iter->second->dActiveTime = ev_now(m_loop);
                        AddIoWriteEvent(pConn);
                    }
                    else
                    {
                        LOG4_TRACE("write len %d, error %d: %s",iWriteLen, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        conn_iter->second->dActiveTime = ev_now(m_loop);
                        AddIoWriteEvent(pConn);
                    }
                }
                else if (iWriteLen > 0)
                {
                    if (pStep != nullptr)
                    {
                    	mapHttpAttr[stMsgShell.iFd].push_back(pStep->GetSequence());
                    }
//                    else
//                    {
//                    	mapHttpAttr[stMsgShell.iFd] = std::list<uint32>();
//                    }
                    iSendByte += iWriteLen;
                    pConn->dActiveTime = ev_now(m_loop);
                    if (iWriteLen == iNeedWriteLen)  // 已无内容可写，取消监听fd写事件
                    {
                        LOG4_TRACE("need write len %d, and had writen len %d", iNeedWriteLen, iWriteLen);
                        RemoveIoWriteEvent(pConn);
                    }
                    else    // 内容未写完，添加或保持监听fd写事件
                    {
                        AddIoWriteEvent(pConn);
                    }
                }
                return(true);
            }
            else
            {
                return(false);
            }
        }
        else
        {
            LOG4_ERROR("fd %d sequence %u not match the ifd %d sequence %u in mapFdAttr",
                            stMsgShell.iFd, stMsgShell.ulSeq, conn_iter->second->iFd,conn_iter->second->ulSeq);
            return(false);
        }
    }
}

bool Worker::SentTo(const std::string& strHost, int iPort, const std::string& strUrlPath, const HttpMsg& oHttpMsg, Step* pStep)
{
    char szIdentify[256] = {0};
    snprintf(szIdentify, sizeof(szIdentify), "%s:%d%s", strHost.c_str(), iPort, strUrlPath.c_str());
    LOG4_TRACE("%s(identify: %s)", __FUNCTION__, szIdentify);
    return(AutoSend(strHost, iPort, strUrlPath, oHttpMsg, pStep));
    // 向外部发起http请求不复用连接
}

bool Worker::SetConnectIdentify(const tagMsgShell& stMsgShell, const std::string& strIdentify)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (stMsgShell.iFd == 0 || strIdentify.size() == 0)
    {
        LOG4_WARN("%s() stMsgShell.iFd(%u) == 0 || strIdentify.size(%zu) == 0",__FUNCTION__,stMsgShell.iFd,strIdentify.size());
        return(false);
    }
    auto iter = mapFdAttr.find(stMsgShell.iFd);
    if (iter == mapFdAttr.end())
    {
        LOG4_ERROR("%s() no fd %d found in mapFdAttr",__FUNCTION__, stMsgShell.iFd);
        return(false);
    }
    else
    {
        if (iter->second->ulSeq == stMsgShell.ulSeq)
        {
        	iter->second->strIdentify = strIdentify;
        	return(true);
        }
        else
        {
            LOG4_ERROR("%s() fd %d sequence %u not match the sequence %u in mapFdAttr",
                            __FUNCTION__,stMsgShell.iFd, stMsgShell.ulSeq, iter->second->ulSeq);
            return(false);
        }
    }
}

bool Worker::AutoSend(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(%s)", __FUNCTION__, strIdentify.c_str());
    int iPosIpPortSeparator = strIdentify.find(':');
    if (iPosIpPortSeparator == std::string::npos)
    {
    	LOG4_ERROR("iPosIpPortSeparator == std::string::npos");
        return(false);
    }

    if (!nodesMgr.IsAlive(strIdentify))//只有在创建连接时检查，因为失活的连接会被移除，不需要在存活时一直检查
	{
    	LOG4_ERROR("strIdentify %s is not alive", strIdentify.c_str());
		return false;
	}

    int iPosPortWorkerIndexSeparator = strIdentify.rfind('.');
    std::string strHost = strIdentify.substr(0, iPosIpPortSeparator);
    std::string strPort = strIdentify.substr(iPosIpPortSeparator + 1, iPosPortWorkerIndexSeparator - (iPosIpPortSeparator + 1));
    std::string strWorkerIndex = strIdentify.substr(iPosPortWorkerIndexSeparator + 1, std::string::npos);
    int iPort = atoi(strPort.c_str());
	if (iPort == 0)
	{
		LOG4_ERROR("%d == 0",iPort);
		return(false);
	}
	int iWorkerIndex = atoi(strWorkerIndex.c_str());
	if (iWorkerIndex > 1000)
	{
		LOG4_ERROR("%d  > 200",iWorkerIndex);
		return(false);
	}
    struct sockaddr addr;
	int iFd = -1;
	if (!HostPort2SockAddr(strHost,iPort,addr,iFd))
	{
		LOG4_ERROR("!HostPort2SockAddr");
		return(false);
	}
	uint32 ulSeq = GetFdSequence();
    tagConnectionAttr* pConn = CreateConnectFdAttr(iFd, ulSeq,strIdentify);
    if (!pConn)// 没有足够资源分配给新连接，直接close掉
	{
    	LOG4_WARN("CreateConnectFdAttr null");
		return(false);
	}
	auto codec_iter = mapCodec.find(pConn->eCodecType);
	if (codec_iter == mapCodec.end())
	{
		LOG4_ERROR("no codec found for %d!", pConn->eCodecType);
		DestroyConnect(mapFdAttr.find(iFd));
		return(false);
	}
	E_CODEC_STATUS eCodecStatus = EncodeByConnectionCodec(pConn, codec_iter->second.get(), oMsgHead, oMsgBody, pConn->pWaitForSendBuff.get());
	if (CODEC_STATUS_OK == eCodecStatus)
	{
		++iSendNum;
	}
	else
	{
		return(false);
	}
	//LOG4_TRACE("fd %d, iter->second->pWaitForSendBuff->ReadableBytes()=%d",iFd, iter->second->pWaitForSendBuff->ReadableBytes());
	mapSeq2WorkerIndex.insert(std::make_pair(ulSeq, iWorkerIndex));
	tagMsgShell stMsgShell(iFd,ulSeq);
	AddMsgShell(strIdentify, stMsgShell);
	connect(iFd, &addr, sizeof(addr));
	return(true);
}

bool Worker::AutoSend(const std::string& strHost, int iPort, const std::string& strUrlPath, const HttpMsg& oHttpMsg, Step* pStep)
{
    LOG4_TRACE("%s(%s, %d, %s)", __FUNCTION__, strHost.c_str(), iPort, strUrlPath.c_str());
    struct sockaddr addr;
	int iFd = -1;
	if (!HostPort2SockAddr(strHost,iPort,addr,iFd))
	{
		LOG4_ERROR("!HostPort2SockAddr");
		return(false);
	}
	tagMsgShell stMsgShell(iFd,GetFdSequence());
    util::E_CODEC_TYPE eHttpCodecType = util::CODEC_HTTP;
    std::string strScheme = strUrlPath.substr(0, strUrlPath.find_first_of(':'));
    std::transform(strScheme.begin(), strScheme.end(), strScheme.begin(),
            [](unsigned char c) -> unsigned char { return std::tolower(c); });
    if (strScheme == std::string("https"))
    {
        eHttpCodecType = util::CODEC_HTTPS;
    }
    tagConnectionAttr* pConnAttr = CreateHttpFdAttr(stMsgShell.iFd, stMsgShell.ulSeq, strHost, eHttpCodecType);
    if (!pConnAttr)
	{
    	LOG4_ERROR("CreateHttpFdAttr null");
    	return(false);
	}
    auto codec_iter = mapCodec.find(pConnAttr->eCodecType);
	if (codec_iter == mapCodec.end())
	{
		LOG4_ERROR("no codec found for %d!", pConnAttr->eCodecType);
		DestroyConnect(mapFdAttr.find(iFd));
		return(false);
	}
	E_CODEC_STATUS eCodecStatus = EncodeByConnectionCodec(pConnAttr, codec_iter->second.get(), oHttpMsg, pConnAttr->pWaitForSendBuff.get());
	if (CODEC_STATUS_OK == eCodecStatus)
	{
		++iSendNum;
	}
	else
	{
		LOG4_ERROR("Encode error");
		DestroyConnect(mapFdAttr.find(iFd));
		return(false);
	}
	connect(stMsgShell.iFd, &addr, sizeof(addr));
	if (pStep != nullptr)
	{
		mapHttpAttr[stMsgShell.iFd].push_back(pStep->GetSequence());
	}
    // 向外部发起http请求不复用连接
	return(true);

}

bool Worker::AutoRedisCmd(const std::string& strHost, int iPort, RedisStep* pRedisStep,const std::string &strPassword)
{
    LOG4_TRACE("%s() redisAsyncConnect(%s, %d)", __FUNCTION__, strHost.c_str(), iPort);
    redisAsyncContext *c = redisAsyncConnect(strHost.c_str(), iPort);
    if (c == nullptr)
    {
        LOG4_ERROR("%s() redisAsyncConnect returned nullptr", __FUNCTION__);
        return(false);
    }
    if (c->err)
    {
        LOG4_ERROR("error: %s", c->errstr);
        redisAsyncFree(c);
        return(false);
    }
    c->data = this;
    tagRedisAttr* pRedisAttr = new tagRedisAttr();
    pRedisAttr->ulSeq = GetFdSequence();
    pRedisAttr->listWaitData.push_back(std::unique_ptr<RedisStep>(pRedisStep));
    if (strPassword.size() > 0)
    {
    	pRedisAttr->strPassword = strPassword;
    }
    pRedisStep->SetRegistered();
    mapRedisAttr.insert(std::make_pair(c, pRedisAttr));
//    LOG4_TRACE("redisLibevAttach(0x%p, 0x%p)", m_loop, c);
    redisLibevAttach(m_loop, c);
//    LOG4_TRACE("redisAsyncSetConnectCallback(0x%p, 0x%p)", c, RedisConnectCallback);
    redisAsyncSetConnectCallback(c, RedisConnectCallback);
//    LOG4_TRACE("redisAsyncSetDisconnectCallback(0x%p, 0x%p)", c, RedisDisconnectCallback);
    redisAsyncSetDisconnectCallback(c, RedisDisconnectCallback);
//    LOG4_TRACE("RedisStep::AddRedisContextAddr(%s, %d, 0x%p)", strHost.c_str(), iPort, c);
    AddRedisContextAddr(strHost, iPort, c);
    return(true);
}

bool Worker::AutoRedisCluster(const std::string& sAddrList, RedisStep* pRedisStep)
{
    LOG4_TRACE("%s(%s)", __FUNCTION__, sAddrList.c_str());
    //sAddrList "192.168.18.78:6000,192.168.18.78:6001,192.168.18.78:6002,192.168.18.78:6003,192.168.18.78:6004,192.168.18.78:6005"
    if (sAddrList.size() == 0 || nullptr == pRedisStep)
    {
        return false;
    }
    redisClusterAsyncContext *acc(nullptr);
    std::unordered_map<std::string,redisClusterAsyncContext*>::iterator it = mapRedisClusterContext.find(sAddrList);
    if (it == mapRedisClusterContext.end())
    {
        acc = redisClusterAsyncConnect(sAddrList.c_str(),HIRCLUSTER_FLAG_NULL);
        if (acc == nullptr)
        {
            LOG4_ERROR("%s() redisClusterAsyncConnect returned nullptr", __FUNCTION__);
            return(false);
        }
        if (acc->err)
        {
            LOG4_ERROR("error: %s", acc->errstr);
            redisClusterAsyncFree(acc);
            return(false);
        }
        LOG4_TRACE("%s new redisClusterAsyncConnect(%s,%d)", __FUNCTION__,acc->cc->ip,acc->cc->port);
        mapRedisClusterContext.insert(std::make_pair(sAddrList,acc));
        mapRedisClusterContextIdentify.insert(std::make_pair(acc,sAddrList));
        redisClusterLibevAttach(acc,m_loop);
        redisClusterAsyncSetConnectCallback(acc,RedisClusterConnectCallback);//void connectCallback(const redisAsyncContext *c, int status)
        redisClusterAsyncSetDisconnectCallback(acc,RedisClusterDisconnectCallback);//void disconnectCallback(const redisAsyncContext *c, int status)
    }
    else
    {
        acc = it->second;
        LOG4_TRACE("%s use redisClusterAsyncConnect(%s,%d)", __FUNCTION__,acc->cc->ip,acc->cc->port);
    }
    {//format cmd ,直接发送，在客户端api内已有发送缓存
        size_t args_size = pRedisStep->GetRedisCmd()->GetCmdArguments().size() + 1;
        const char* argv[args_size];
        size_t arglen[args_size];
        argv[0] = pRedisStep->GetRedisCmd()->GetCmd().c_str();
        arglen[0] = pRedisStep->GetRedisCmd()->GetCmd().size();
        std::vector<std::pair<std::string, bool> >::const_iterator c_iter = pRedisStep->GetRedisCmd()->GetCmdArguments().begin();
        for (size_t i = 1; c_iter != pRedisStep->GetRedisCmd()->GetCmdArguments().end(); ++c_iter, ++i)
        {
            argv[i] = c_iter->first.c_str();
            arglen[i] = c_iter->first.size();
        }
        // redisAsyncContext->data 存 Worker；RedisClusterCmdCallback 的 privdata 为传入的 this
        int iCmdStatus = redisClusterAsyncCommandArgv(acc, RedisClusterCmdCallback, this, args_size, argv, arglen);
        if (iCmdStatus == REDIS_OK)
        {
            LOG4_TRACE("succeed in sending redis cmd: %s",pRedisStep->GetRedisCmd()->ToString().c_str());
            tagRedisAttr* ptagRedisAttr(nullptr);
            std::unordered_map<redisClusterAsyncContext*, tagRedisAttr*>::iterator acc_iter = mapRedisClusterAttr.find(acc);
            if (acc_iter == mapRedisClusterAttr.end())
            {
                ptagRedisAttr = new tagRedisAttr();
                ptagRedisAttr->bIsReady = true;
                mapRedisClusterAttr.insert(std::make_pair(acc,ptagRedisAttr));
            }
            else
            {
                ptagRedisAttr = acc_iter->second;
            }
            ptagRedisAttr->listData.push_back(std::unique_ptr<RedisStep>(pRedisStep));
        }
        else    // 命令执行失败，不再继续执行，等待下一次回调
        {
            LOG4_WARN("failed in sending redis cmd: %s,err:%d,errstr:%s", pRedisStep->GetRedisCmd()->ToString().c_str(),acc->err,acc->errstr);
            redisAsyncContext cobj;//对逻辑层只是抛出错误信息，不直接使用连接对象
            cobj.err = acc->err;
            cobj.errstr = acc->errstr;
            redisAsyncContext *c = &cobj;
            pRedisStep->AddCallBackCounter();
            if (STATUS_CMD_RUNNING != pRedisStep->Callback(c, acc->err, NULL))
            {
            	SAFE_DELETE(pRedisStep);
            }
        }
    }
    return true;
}

bool Worker::AutoMysqlCmd(MysqlStep* pMysqlStep)
{
	LOG4_TRACE("%s() AutoMysqlCmd(%s, %d)", __FUNCTION__, pMysqlStep->m_strHost.c_str(), pMysqlStep->m_iPort);
	struct ev_loop *loop = m_loop;
	util::MysqlAsyncConn * pConn = util::mysqlAsyncConnect(pMysqlStep->m_strHost.c_str(),
			pMysqlStep->m_iPort,pMysqlStep->m_strUser.c_str(),pMysqlStep->m_strPasswd.c_str(),
			pMysqlStep->m_strDbName.c_str(),pMysqlStep->m_dbcharacterset.c_str(),loop);
	pMysqlStep->SetRegistered();
	AddMysqlContextAddr(pMysqlStep,pConn);
	return RegisterCallback(pConn, pMysqlStep);
}

bool Worker::AutoConnect(const std::string& strIdentify)
{
    LOG4_TRACE("%s(%s)", __FUNCTION__, strIdentify.c_str());
    int iPosIpPortSeparator = strIdentify.find(':');
    if (iPosIpPortSeparator == std::string::npos)
    {
    	LOG4_ERROR("iPosIpPortSeparator == std::string::npos");
        return(false);
    }
    int iPosPortWorkerIndexSeparator = strIdentify.rfind('.');
    std::string strHost = strIdentify.substr(0, iPosIpPortSeparator);
    std::string strPort = strIdentify.substr(iPosIpPortSeparator + 1, iPosPortWorkerIndexSeparator - (iPosIpPortSeparator + 1));
    std::string strWorkerIndex = strIdentify.substr(iPosPortWorkerIndexSeparator + 1, std::string::npos);
    int iPort = atoi(strPort.c_str());
    if (iPort == 0)
    {
    	LOG4_ERROR("iPort(%d) == 0",iPort);
        return(false);
    }
    int iWorkerIndex = atoi(strWorkerIndex.c_str());
    if (iWorkerIndex > 1000)
    {
    	LOG4_ERROR("iWorkerIndex(%d) > 200",iWorkerIndex);
        return(false);
    }
    struct sockaddr addr;
	int iFd = -1;
	if (!HostPort2SockAddr(strHost,iPort,addr,iFd))
	{
		LOG4_ERROR("!HostPort2SockAddr");
		return(false);
	}
    uint32 ulSeq = GetFdSequence();
    tagConnectionAttr* pConn = CreateConnectFdAttr(iFd,ulSeq,strIdentify);
	if (!pConn)
	{
		LOG4_ERROR("CreateConnectFdAttr error");
		return(false);
	}
	mapSeq2WorkerIndex.insert(std::make_pair(ulSeq, iWorkerIndex));
	tagMsgShell stMsgShell(iFd,ulSeq);
	AddMsgShell(strIdentify, stMsgShell);
	connect(iFd, &addr, sizeof(addr));
	return(true);
}

bool Worker::HttpsGet(const std::string & strUrl, std::string & strResponse,
        const std::string& strUserpwd,util::CurlClient::eContentType eType, const std::string& strCaPath,int iPort)
{
    util::CurlClient curlClient;
    CURLcode res = curlClient.GetHttps(strUrl,strResponse,strUserpwd,eType,strCaPath,iPort);
    if (CURLE_OK != res)
    {
        LOG4_WARN("%s() CURLcode(%d,%s) strUrl:%s",__FUNCTION__,res,curl_easy_strerror(res),strUrl.c_str());
        return false;
    }
    return true;
}
bool Worker::HttpsPost(const std::string & strUrl, const std::string & strFields,
        std::string & strResponse,const std::string& strUserpwd,util::CurlClient::eContentType eType,const std::string& strCaPath,int iPort)
{
	util::CurlClient curlClient;
    CURLcode res = curlClient.PostHttps(strUrl,strFields,strResponse,strUserpwd,eType,strCaPath,iPort);
    if (CURLE_OK != res)
    {
        LOG4_WARN("%s() CURLcode(%d,%s) strUrl:%s",__FUNCTION__,res,curl_easy_strerror(res),strUrl.c_str());
        return false;
    }
    return true;
}
void Worker::AddInnerFd(const tagMsgShell& stMsgShell)
{
	iInnerFdCounter++;
    LOG4_TRACE("%s() iInnerFdCounter = %u", __FUNCTION__, iInnerFdCounter);
}

void Worker::DelInnerFd(const tagMsgShell& stMsgShell)
{
	if (iInnerFdCounter > 0)
	{
		iInnerFdCounter--;
	}
}


bool Worker::GetMsgShell(const std::string& strIdentify, tagMsgShell& stMsgShell)
{
    LOG4_TRACE("%s(identify: %s)", __FUNCTION__, strIdentify.c_str());
    if (!GetMsgShell(strIdentify, stMsgShell))
    {
        LOG4_TRACE("no tagMsgShell match %s.", strIdentify.c_str());
        return(false);
    }
    return(true);
}

bool Worker::SetClientData(const tagMsgShell& stMsgShell, util::CBuffer* pBuff)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter = mapFdAttr.find(stMsgShell.iFd);
    if (iter == mapFdAttr.end())
    {
        return(false);
    }
    else
    {
        if (iter->second->ulSeq == stMsgShell.ulSeq)
        {
        	if (iter->second->pClientData->ReadableBytes() > 0)
        	{
        		iter->second->pClientData->Clear();
        	}
            iter->second->pClientData->Write(pBuff, pBuff->ReadableBytes());
            return(true);
        }
        else
        {
            return(false);
        }
    }
}

bool Worker::HadClientData(const tagMsgShell& stMsgShell)
{
    auto conn_iter = mapFdAttr.find(stMsgShell.iFd);
    if (conn_iter == mapFdAttr.end())
    {
        return(false);
    }
	if (stMsgShell.ulSeq != conn_iter->second->ulSeq)
	{
		return(false);
	}
	if (conn_iter->second->pClientData != nullptr && conn_iter->second->pClientData->ReadableBytes() > 0)
	{
		return(true);
	}
	else
	{
		return(false);
	}
}

bool Worker::GetClientData(const tagMsgShell& stMsgShell, util::CBuffer* pBuff)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	auto iter = mapFdAttr.find(stMsgShell.iFd);
	if (iter == mapFdAttr.end())
	{
		return(false);
	}
	if (iter->second->ulSeq == stMsgShell.ulSeq)
	{
		if (iter->second->pClientData != nullptr && iter->second->pClientData->ReadableBytes() > 0)
		{
			pBuff->Write(iter->second->pClientData->GetRawReadBuffer(), iter->second->pClientData->ReadableBytes());
			return(true);
		}
	}
	return(false);
}

bool Worker::SetSessionKey(const tagMsgShell& stMsgShell,const std::string &strSessionKey)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (strSessionKey.size() == 0)
    {
    	return(false);
    }
    auto iter = mapFdAttr.find(stMsgShell.iFd);
	if (iter == mapFdAttr.end())
	{
		return(false);
	}
	else
	{
		if (iter->second->ulSeq == stMsgShell.ulSeq)
		{
			iter->second->pRecvBuff->strSessionKey = iter->second->pWaitForSendBuff->strSessionKey
					= iter->second->pSendBuff->strSessionKey = iter->second->strSessionKey = strSessionKey;
			return(true);
		}
		else
		{
			return(false);
		}
	}
}

std::string Worker::GetClientAddr(const tagMsgShell& stMsgShell)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter = mapFdAttr.find(stMsgShell.iFd);
    if (iter == mapFdAttr.end())
    {
        return("");
    }
	if (iter->second->ulSeq == stMsgShell.ulSeq)
	{
		return(iter->second->szRemoteAddr);
	}
	else
	{
		return("");
	}
}

bool Worker::SetAdditionalData(const tagMsgShell& stMsgShell,MsgBody& oMsgBody)
{
	auto iter = mapFdAttr.find(stMsgShell.iFd);
	if (iter == mapFdAttr.end())
	{
		LOG4_WARN("%s() iter == mapFdAttr.end() error",__FUNCTION__);
		return(false);
	}
	if (iter->second->ulSeq == stMsgShell.ulSeq)
	{
		if (iter->second->pClientData != nullptr && iter->second->pClientData->ReadableBytes() > 0)
		{
			oMsgBody.set_additional(iter->second->pClientData->GetRawReadBuffer(),iter->second->pClientData->ReadableBytes());
			return true;
		}
	}
	LOG4_WARN("%s() SetAdditionalData error",__FUNCTION__);
	return false;
}


bool Worker::SetAdditionalData(const tagConnectionAttr* pConn,MsgBody& oMsgBody)
{
	if (pConn->pClientData != nullptr && pConn->pClientData->ReadableBytes() > 0)
	{
		oMsgBody.set_additional(pConn->pClientData->GetRawReadBuffer(),pConn->pClientData->ReadableBytes());
		return true;
	}
	LOG4_WARN("%s() SetAdditionalData error",__FUNCTION__);
	return false;
}

std::string Worker::GetConnectIdentify(const tagMsgShell& stMsgShell)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter = mapFdAttr.find(stMsgShell.iFd);
    if (iter == mapFdAttr.end())
    {
        return("");
    }
	if (iter->second->ulSeq == stMsgShell.ulSeq)
	{
		return iter->second->strIdentify;
	}
	else
	{
		return("");
	}
}

bool Worker::AbandonConnect(const std::string& strIdentify)
{
    LOG4_TRACE("%s(identify: %s)", __FUNCTION__, strIdentify.c_str());
    auto shell_iter = mapMsgShell.find(strIdentify);
    if (shell_iter == mapMsgShell.end())
    {
        LOG4_TRACE("no tagMsgShell match %s.", strIdentify.c_str());
        return(false);
    }
    else
    {
        auto iter = mapFdAttr.find(shell_iter->second.iFd);
        if (iter == mapFdAttr.end())
        {
            return(false);
        }
        else
        {
            if (iter->second->ulSeq == shell_iter->second.ulSeq)
            {
                iter->second->strIdentify = "";
                iter->second->pClientData->Clear();
                mapMsgShell.erase(shell_iter);
                return(true);
            }
            else
            {
                return(false);
            }
        }
        return(true);
    }
}

bool Worker::SetHeartBeat(const tagMsgShell& stMsgShell)
{
	if (nodesMgr.IsCheckInternalRouter())
	{
		auto iter = mapFdAttr.find(stMsgShell.iFd);
		if (iter == mapFdAttr.end())
		{
			return(false);
		}
		if (iter->second->ulSeq == stMsgShell.ulSeq)
		{
			LOG4_TRACE("%s() strIdentify(%s)",__FUNCTION__,iter->second->strIdentify.c_str());
			return nodesMgr.SetHeartBeat(iter->second->strIdentify);
		}
		else
		{
			return(false);
		}
	}
	return(true);
}

bool Worker::CheckHeartBeat(const tagMsgShell& stMsgShell)
{
	if (nodesMgr.IsCheckInternalRouter())
	{
		auto iter = mapFdAttr.find(stMsgShell.iFd);
		if (iter == mapFdAttr.end())
		{
			return(false);
		}
		if (iter->second->ulSeq == stMsgShell.ulSeq)
		{
			bool bCheckHeartBeat = nodesMgr.CheckHeartBeat(iter->second->strIdentify);
			LOG4_TRACE("%s() strIdentify(%s) bCheckHeartBeat(%d)",__FUNCTION__,iter->second->strIdentify.c_str(),bCheckHeartBeat);
			return bCheckHeartBeat;
		}
		else
		{
			return(false);
		}
	}
	return(true);
}

const tagConnectionAttr* Worker::GetConnectionAttr(const tagMsgShell& stMsgShell)
{
	auto iter = mapFdAttr.find(stMsgShell.iFd);
	if (iter == mapFdAttr.end())
	{
		LOG4_WARN("%s() iter == mapFdAttr.end() error",__FUNCTION__);
		return(nullptr);
	}
	if (iter->second->ulSeq == stMsgShell.ulSeq)
	{
		return iter->second.get();
	}
	LOG4_WARN("%s() iter->second->ulSeq(%u) == stMsgShell.ulSeq(%u) error",__FUNCTION__,iter->second->ulSeq,stMsgShell.ulSeq);
	return(nullptr);
}

bool Worker::ExecStep(uint32 uiCallerStepSeq, uint32 uiCalledStepSeq,int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    LOG4_TRACE("%s(caller[%u], called[%u])", __FUNCTION__, uiCallerStepSeq, uiCalledStepSeq);
    auto step_iter = mapCallbackStep.find(uiCalledStepSeq);
    if (step_iter == mapCallbackStep.end())
    {
        LOG4_WARN("step %u is not in the callback list.", uiCalledStepSeq);
    }
    else
    {
    	int nRet = step_iter->second->Emit(iErrno, strErrMsg, strErrShow);
        if (net::STATUS_CMD_RUNNING != nRet)
        {
            DeleteCallback(step_iter->second.get());
        }
        if (nRet != net::STATUS_CMD_FAULT)return true;
    }
    return false;
}

bool Worker::ExecStep(uint32 uiStepSeq,int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    LOG4_TRACE("%s(uiStepSeq[%u])", __FUNCTION__,uiStepSeq);
    auto step_iter = mapCallbackStep.find(uiStepSeq);
    if (step_iter == mapCallbackStep.end())
    {
        LOG4_WARN("step %u is not in the callback list.", uiStepSeq);
    }
    else
    {
    	int nRet = step_iter->second->Emit(iErrno, strErrMsg, strErrShow);
        if (net::STATUS_CMD_RUNNING != nRet)
        {
            DeleteCallback(step_iter->second.get());
        }
        if (nRet != net::STATUS_CMD_FAULT)return true;
    }
    return false;
}

bool Worker::ExecStep(Step* pStep,ev_tstamp dTimeout,int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
	if (!pStep)
	{
		LOG4_ERROR("%s() null pStep",__FUNCTION__);
		return false;
	}
	if (!pStep->IsRegistered())
	{
		if (!RegisterCallback(pStep,dTimeout))
		{
			LOG4_ERROR("%s() RegisterCallback error",__FUNCTION__);
			SAFE_DELETE(pStep);
			return(false);
		}
		LOG4_TRACE("%s(RegisterCallback[%u])", __FUNCTION__,pStep->GetSequence());
	}
    LOG4_TRACE("%s(uiStepSeq[%u])", __FUNCTION__,pStep->GetSequence());
    auto step_iter = mapCallbackStep.find(pStep->GetSequence());
    if (step_iter == mapCallbackStep.end())
    {
        LOG4_WARN("step %u is not in the callback list.", pStep->GetSequence());
    }
    else
    {
    	int nRet = step_iter->second->Emit(iErrno, strErrMsg, strErrShow);
        if (net::STATUS_CMD_RUNNING != nRet)DeleteCallback(step_iter->second.get());
        if (net::STATUS_CMD_FAULT != nRet)return true;
        LOG4_ERROR("step Emit failed.");
    }
    return false;
}

bool Worker::ExecStep(RedisStep* pRedisStep)
{
	if (nullptr == pRedisStep)
	{
		LOG4_ERROR("nullptr == pRedisStep");
		return(false);
	}
	LOG4_TRACE("%s() pRedisStep",__FUNCTION__);
	if (net::STATUS_CMD_RUNNING == pRedisStep->Emit(ERR_OK))//RedisStep注册在其Emit内实现
	{
		return(true);
	}
	LOG4_WARN("%s() pRedisStep",__FUNCTION__);
	SAFE_DELETE(pRedisStep);
	return false;
}

Step* Worker::GetStep(uint32 uiStepSeq)
{
	auto iter = mapCallbackStep.find(uiStepSeq);
	if (iter == mapCallbackStep.end())
	{
		return nullptr;
	}
	return iter->second.get();
}

bool Worker::IsRegisteredStep(uint32 uiStepSeq, const Step* pStep)
{
	auto iter = mapCallbackStep.find(uiStepSeq);
	if (iter == mapCallbackStep.end())
	{
		return false;
	}
	return iter->second.get() == pStep;
}

void Worker::LoadSo(util::CJsonObject& oSoConf,bool boForce)
{
    LOG4_TRACE("%s():oSoConf(%s)", __FUNCTION__,oSoConf.ToString().c_str());
    int iCmd = 0;
    int iVersion = 0;
    bool bIsload = false;
    std::string strSoPath;
    std::unordered_map<int, std::unique_ptr<tagSo>>::iterator cmd_iter;
    tagSo* pSo = nullptr;
    for (int i = 0; i < oSoConf.GetArraySize(); ++i)
    {
        oSoConf[i].Get("load", bIsload);
        if (bIsload)
        {
            strSoPath = m_strWorkPath + std::string("/") + oSoConf[i]("so_path");
            if (oSoConf[i].Get("cmd", iCmd) && oSoConf[i].Get("version", iVersion))
            {
                if (0 != access(strSoPath.c_str(), F_OK))
                {
                    LOG4_WARN("%s not exist!", strSoPath.c_str());
                    continue;
                }
                cmd_iter = mapSo.find(iCmd);
                if (cmd_iter == mapSo.end())
                {
                    LOG4_INFO("try to load:%s", strSoPath.c_str());
                    LoadSoAndGetCmd(iCmd, strSoPath, oSoConf[i]("entrance_symbol"), iVersion);
                }
                else
                {
                    if (iVersion != cmd_iter->second->iVersion || boForce)
                    {
                        LoadSoAndGetCmd(iCmd, strSoPath, oSoConf[i]("entrance_symbol"), iVersion);
                    }
                    else
                    {
                        LOG4_INFO("same version(%d).no need  to load %s",cmd_iter->second->iVersion,
                                        strSoPath.c_str());
                    }
                }
            }
            else
            {
                LOG4_WARN("cmd or version not found.failed to load %s", strSoPath.c_str());
            }
        }
        else        // 卸载动态库
        {
            strSoPath = m_strWorkPath + std::string("/") + oSoConf[i]("so_path");
            if (oSoConf[i].Get("cmd", iCmd))
            {
                LOG4_INFO("unload %s", strSoPath.c_str());
                UnloadSoAndDeleteCmd(iCmd);
            }
            else
            {
                LOG4_WARN("cmd not exist.failed to unload %s", strSoPath.c_str());
            }
        }
    }
}

void Worker::ReloadSo(util::CJsonObject& oCmds)
{
    LOG4_TRACE("%s():oCmds(%s)", __FUNCTION__,oCmds.ToString().c_str());
    int iCmd = 0;
    int iVersion = 0;
    std::string strSoPath;
    std::string strSymbol;
    std::unordered_map<int, std::unique_ptr<tagSo>>::iterator cmd_iter;
    tagSo* pSo = nullptr;
    for (int i = 0; i < oCmds.GetArraySize(); ++i)
    {
        std::string cmd = oCmds[i].ToString();
//        std::string::iterator it = std::remove(cmd.begin(), cmd.end(), '\"');
//        cmd.erase(it, cmd.end());
        iCmd = atoi(cmd.c_str());
        cmd_iter = mapSo.find(iCmd);
        if (cmd_iter != mapSo.end())
        {
            strSoPath = cmd_iter->second->strSoPath;
            strSymbol = cmd_iter->second->strSymbol;
            iVersion = cmd_iter->second->iVersion;
            if (0 != access(strSoPath.c_str(), F_OK))
            {
                LOG4_WARN("%s not exist!", strSoPath.c_str());
                continue;
            }
            LoadSoAndGetCmd(iCmd, strSoPath, strSymbol, iVersion);
        }
        else
        {
            LOG4_WARN("no such cmd %s", cmd.c_str());
        }
    }
}

tagSo* Worker::LoadSoAndGetCmd(int iCmd, const std::string& strSoPath, const std::string& strSymbol, int iVersion)
{
    LOG4_TRACE("%s() iCmd:%d", __FUNCTION__,iCmd);
    UnloadSoAndDeleteCmd(iCmd);
    tagSo* pSo = nullptr;
    void* pHandle = nullptr;
    pHandle = dlopen(strSoPath.c_str(),RTLD_NOW|RTLD_NODELETE);
    char* dlsym_error = dlerror();
    if (dlsym_error)
    {
        LOG4_FATAL("cannot load dynamic lib %s!" , dlsym_error);
        if (pHandle != nullptr)
        {
            dlclose(pHandle);
        }
        return(pSo);
    }
    CreateCmd* pCreateCmd = (CreateCmd*)dlsym(pHandle, strSymbol.c_str());
    dlsym_error = dlerror();
    if (dlsym_error)
    {
        LOG4_FATAL("dlsym error %s!" , dlsym_error);
        dlclose(pHandle);
        return(pSo);
    }
    Cmd* pCmd = pCreateCmd();
    LOG4_TRACE("%s() strSoPath:%s pHandle:%p pCreateCmd:%p pCmd:%p",__FUNCTION__,strSoPath.c_str(),pHandle,pCreateCmd,pCmd);
    if (pCmd != nullptr)
    {
        pSo = new tagSo();
        if (pSo != nullptr)
        {
            pSo->pSoHandle = pHandle;
            pSo->pCmd.reset(pCmd);
            pSo->strSoPath = strSoPath;
            pSo->strSymbol = strSymbol;
            pSo->iVersion = iVersion;
            pSo->pCmd->SetCmd(iCmd);
            if (!pSo->pCmd->Init())
            {
                LOG4_FATAL("Cmd %d %s init error",iCmd, strSoPath.c_str());
                delete pSo;
                return nullptr;
            }
            LOG4_INFO("succeed in loading(%s) strLoadTime(%s) pHandle:%p pCreateCmd:%p pCmd:%p",
                    strSoPath.c_str(),pSo->strLoadTime.c_str(),pHandle,pCreateCmd,pCmd);
            mapSo.insert(std::make_pair(iCmd, std::unique_ptr<tagSo>(pSo)));
            return pSo;
        }
        else
        {
            LOG4_FATAL("new tagSo() error for %s!",strSoPath.c_str());
            delete pCmd;
            dlclose(pHandle);
        }
    }
    return(pSo);
}

void Worker::UnloadSoAndDeleteCmd(int iCmd)
{
    LOG4_TRACE("%s() iCmd:%d", __FUNCTION__,iCmd);
    auto mapSoIt = mapSo.find(iCmd);
    if (mapSoIt != mapSo.end())
    {
        LOG4_INFO("succeed in unloading(%s) strLoadTime(%s),strNowTime(%s)",
                                mapSoIt->second->strSoPath.c_str(),mapSoIt->second->strLoadTime.c_str(),util::GetCurrentTime(20).c_str());
        void* pSoHandle = mapSoIt->second->pSoHandle;
        if(pSoHandle)
        {
            LOG4_TRACE("%s() dlclose iCmd:%d", __FUNCTION__,iCmd);
            dlclose(pSoHandle);
            pSoHandle = nullptr;
        }
        mapSo.erase(mapSoIt);
    }
}

void Worker::LoadModule(util::CJsonObject& oModuleConf,bool boForce)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    std::string strModulePath;
    int iVersion = 0;
    bool bIsload = false;
    std::string strSoPath;
    std::unordered_map<std::string, std::unique_ptr<tagModule>>::iterator module_iter;
    tagModule* pModule = nullptr;
    LOG4_TRACE("oModuleConf.GetArraySize() = %d,oModuleConf(%s)", oModuleConf.GetArraySize(),
                    oModuleConf.ToString().c_str());
    for (int i = 0; i < oModuleConf.GetArraySize(); ++i)
    {
        oModuleConf[i].Get("load", bIsload);
        if (bIsload)
        {
            if (oModuleConf[i].Get("url_path", strModulePath) && oModuleConf[i].Get("version", iVersion))
            {
                LOG4_TRACE("url_path = %s", strModulePath.c_str());
                strSoPath = m_strWorkPath + std::string("/") + oModuleConf[i]("so_path");
                if (0 != access(strSoPath.c_str(), F_OK))
                {
                    LOG4_WARN("%s not exist!", strSoPath.c_str());
                    continue;
                }
                module_iter = mapModule.find(strModulePath);
                if (module_iter == mapModule.end())
                {
                    LoadSoAndGetModule(strModulePath, strSoPath, oModuleConf[i]("entrance_symbol"), iVersion);
                }
                else
                {
                    if (iVersion != module_iter->second->iVersion || boForce)
                    {
                        LoadSoAndGetModule(strModulePath, strSoPath, oModuleConf[i]("entrance_symbol"), iVersion);
                    }
                    else
                    {
                        LOG4_INFO("already exist same version:%s", strSoPath.c_str());
                    }
                }
            }
        }
        else        // 卸载动态库
        {
            if (oModuleConf[i].Get("url_path", strModulePath))
            {
                strSoPath = m_strWorkPath + std::string("/") + oModuleConf[i]("so_path");
                UnloadSoAndDeleteModule(strModulePath);
                LOG4_INFO("unload %s", strSoPath.c_str());
            }
        }
    }
}

void Worker::ReloadModule(util::CJsonObject& oUrlPaths)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    std::unordered_map<std::string, std::unique_ptr<tagModule>>::iterator module_iter;
    tagModule* pModule = nullptr;
    LOG4_TRACE("oUrlPaths.GetArraySize() = %d,oUrlPaths(%s)", oUrlPaths.GetArraySize(),
                    oUrlPaths.ToString().c_str());
    std::string strSoPath;
    std::string strSymbol;
    int iVersion(0);
    for (int i = 0; i < oUrlPaths.GetArraySize(); ++i)
    {
        std::string url_path = oUrlPaths[i].ToString();
        std::string::iterator it = std::remove(url_path.begin(), url_path.end(), '\"');
        url_path.erase(it, url_path.end());
        LOG4_TRACE("url_path = %s", url_path.c_str());
        module_iter = mapModule.find(url_path);
        if (module_iter != mapModule.end())
        {
            strSoPath = module_iter->second->strSoPath;
            strSymbol = module_iter->second->strSymbol;
            iVersion = module_iter->second->iVersion;
            if (0 != access(strSoPath.c_str(), F_OK))
            {
                LOG4_WARN("%s not exist!", strSoPath.c_str());
                continue;
            }
            LoadSoAndGetModule(url_path, strSoPath, strSymbol, iVersion);
        }
        else
        {
            LOG4_WARN("no such url_path %s", url_path.c_str());
        }
    }
}

tagModule* Worker::LoadSoAndGetModule(const std::string& strModulePath, const std::string& strSoPath, const std::string& strSymbol, int iVersion)
{
    LOG4_TRACE("%s() strModulePath:%s", __FUNCTION__,strModulePath.c_str());
    UnloadSoAndDeleteModule(strModulePath);
    tagModule* pSo = nullptr;
    void* pHandle = nullptr;
    pHandle = dlopen(strSoPath.c_str(), RTLD_NOW|RTLD_NODELETE);
    char* dlsym_error = dlerror();
    if (dlsym_error)
    {
        LOG4_FATAL("cannot load dynamic lib %s!" , dlsym_error);
        return(pSo);
    }
    CreateCmd* pCreateModule = (CreateCmd*)dlsym(pHandle, strSymbol.c_str());
    dlsym_error = dlerror();
    if (dlsym_error)
    {
        LOG4_FATAL("dlsym error %s!" , dlsym_error);
        dlclose(pHandle);
        return(pSo);
    }
    Module* pModule = (Module*)pCreateModule();
    LOG4_TRACE("%s() strSoPath:%s pHandle:%p pCreateModule:%p pModule:%p",
            __FUNCTION__,strSoPath.c_str(),pHandle,pCreateModule,pModule);
    if (pModule != nullptr)
    {
        pSo = new tagModule();
        if (pSo != nullptr)
        {
            pSo->pSoHandle = pHandle;
            pSo->pModule.reset(pModule);
            pSo->strSoPath = strSoPath;
            pSo->strSymbol = strSymbol;
            pSo->iVersion = iVersion;
            pSo->pModule->SetModulePath(strModulePath);
            if (!pSo->pModule->Init())
            {
                LOG4_FATAL("Module %s %s init error", strModulePath.c_str(), strSoPath.c_str());
                delete pSo;
                return nullptr;
            }
            mapModule.insert(std::make_pair(strModulePath, std::unique_ptr<tagModule>(pSo)));
        }
        else
        {
            LOG4_FATAL("new tagSo() error for %s!",strSoPath.c_str());
            delete pModule;
            dlclose(pHandle);
        }
    }
    return(pSo);
}

void Worker::UnloadSoAndDeleteModule(const std::string& strModulePath)
{
    LOG4_TRACE("%s() strModulePath:%s", __FUNCTION__,strModulePath.c_str());
    auto mapMoIt = mapModule.find(strModulePath);
    if (mapMoIt != mapModule.end())
    {
        LOG4_INFO("succeed in unloading(%s) strLoadTime(%s),strNowTime(%s)",
                mapMoIt->second->strSoPath.c_str(),mapMoIt->second->strLoadTime.c_str(),util::GetCurrentTime(20).c_str());
        void* pSoHandle = mapMoIt->second->pSoHandle;
        if(pSoHandle)
        {
            LOG4_TRACE("%s() dlclose strModulePath:%s", __FUNCTION__, strModulePath.c_str());
            dlclose(pSoHandle);
            pSoHandle = nullptr;
        }
        mapModule.erase(mapMoIt);
    }
}

bool Worker::AddPeriodicTaskEvent()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    {
    	ev_timer* timeout_watcher = new ev_timer();
		timeout_watcher->data = (void*)this;
		AddEvent(NODE_BEAT,timeout_watcher,PeriodicTaskCallback);
    }
    {
    	ev_timer* timeout_watcher = new ev_timer();
		timeout_watcher->data = (void*)this;
		AddEvent(1.0,timeout_watcher,ShortPeriodicTaskCallback);
    }
    return(true);
}

bool Worker::AddIoReadEvent(tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ev_io* io_watcher = nullptr;
	if (nullptr == pConn->pIoWatcher)
	{
		io_watcher = new ev_io();
		if (io_watcher == nullptr)
		{
			LOG4_ERROR("new io_watcher error!");
			return(false);
		}
		tagIoWatcherData* pData = new tagIoWatcherData();
		if (pData == nullptr)
		{
			LOG4_ERROR("new tagIoWatcherData error!");
			delete io_watcher;
			return(false);
		}
		pData->iFd = pConn->iFd;
		pData->ulSeq = pConn->ulSeq;
		pData->pWorker = this;
		io_watcher->data = (void*)pData;
		pConn->pIoWatcher = io_watcher;

		AddEvent(EV_READ,io_watcher,IoCallback,pData->iFd);
	}
	else
	{
		RefreshEvent(pConn->pIoWatcher->events | EV_READ,pConn->pIoWatcher);
	}
    return(true);
}

bool Worker::AddIoWriteEvent(tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ev_io* io_watcher = nullptr;
	if (nullptr == pConn->pIoWatcher)
	{
		io_watcher = new ev_io();
		if (io_watcher == nullptr)
		{
			LOG4_ERROR("new io_watcher error!");
			return(false);
		}
		tagIoWatcherData* pData = new tagIoWatcherData();
		if (pData == nullptr)
		{
			LOG4_ERROR("new tagIoWatcherData error!");
			delete io_watcher;
			return(false);
		}
		pData->iFd = pConn->iFd;
		pData->ulSeq = pConn->ulSeq;
		pData->pWorker = this;

		io_watcher->data = (void*)pData;
		pConn->pIoWatcher = io_watcher;

		AddEvent(EV_WRITE,io_watcher,IoCallback,pData->iFd);
	}
	else
	{
		RefreshEvent(pConn->pIoWatcher->events | EV_WRITE,pConn->pIoWatcher);
	}
    return(true);
}

bool Worker::RemoveIoWriteEvent(tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s()", __FUNCTION__);
	if (NULL != pConn->pIoWatcher)
	{
		if (pConn->pIoWatcher->events & EV_WRITE)
		{
			RefreshEvent(pConn->pIoWatcher->events & ~EV_WRITE,pConn->pIoWatcher);
		}
	}
    return(true);
}

bool Worker::AddIoTimeout(int iFd, uint32 ulSeq, tagConnectionAttr* pConnAttr, ev_tstamp dTimeout)
{
    LOG4_TRACE("%s() szRemoteAddr(%s) dTimeout(%lf)", __FUNCTION__,pConnAttr->szRemoteAddr,dTimeout);
    if (pConnAttr->pTimeWatcher != nullptr)
    {
    	LOG4_TRACE("%s() timer dTimeout(%lf) ev_time(%lf) ev_now(%lf)",__FUNCTION__,m_dIoTimeout,ev_time(),ev_now(m_loop));
    	RefreshEvent(m_dIoTimeout,pConnAttr->pTimeWatcher);
        return(true);
    }
    else
    {
        ev_timer* timeout_watcher = new ev_timer();
        if (timeout_watcher == nullptr)
        {
            LOG4_ERROR("new timeout_watcher error!");
            return(false);
        }
        tagIoWatcherData* pData = new tagIoWatcherData();
        if (pData == nullptr)
        {
            LOG4_ERROR("new tagIoWatcherData error!");
            delete timeout_watcher;
            return(false);
        }
        LOG4_TRACE("%s() timer dTimeout(%lf) ev_time(%lf) ev_now(%lf)",__FUNCTION__,dTimeout,ev_time(),ev_now(m_loop));
        pData->iFd = iFd;
        pData->ulSeq = ulSeq;
        pData->pWorker = this;
        timeout_watcher->data = (void*)pData;
        pConnAttr->pTimeWatcher = timeout_watcher;
        AddEvent(dTimeout,timeout_watcher,IoTimeoutCallback);
        return(true);
    }
}

E_CODEC_STATUS Worker::EncodeByConnectionCodec(tagConnectionAttr* pConn, ThunderCodec* pCodec,
        const MsgHead& oMsgHead, const MsgBody& oMsgBody, util::CBuffer* pBuff)
{
    if (pConn == nullptr || pCodec == nullptr || pBuff == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    if (pConn->eCodecType == util::CODEC_HTTPS)
    {
        return static_cast<HttpsCodec*>(pCodec)->EncodeToConnection(pConn, oMsgHead, oMsgBody, pBuff);
    }
    return pCodec->Encode(oMsgHead, oMsgBody, pBuff);
}

E_CODEC_STATUS Worker::EncodeByConnectionCodec(tagConnectionAttr* pConn, ThunderCodec* pCodec,
        const HttpMsg& oHttpMsg, util::CBuffer* pBuff)
{
    if (pConn == nullptr || pCodec == nullptr || pBuff == nullptr)
    {
        return CODEC_STATUS_ERR;
    }
    if (pConn->eCodecType == util::CODEC_HTTPS)
    {
        return static_cast<HttpsCodec*>(pCodec)->EncodeToConnection(pConn, oHttpMsg, pBuff);
    }
    return pCodec->Encode(oHttpMsg, pBuff);
}

tagConnectionAttr* Worker::CreateConnectFdAttr(int iFd, uint32 ulSeq,const std::string & strIdentify)
{
	tagConnectionAttr* pConn = CreateFdAttr(iFd, ulSeq);
	if (!pConn)// 没有足够资源分配给新连接，直接close掉
	{
		LOG4_ERROR("CreateFdAttr error");
		close(iFd);
		return(nullptr);
	}
	snprintf(pConn->szRemoteAddr, 32, "%s", strIdentify.c_str());
	if (!AddIoTimeout(iFd, ulSeq, pConn, 1.5))
	{
		LOG4_ERROR("if(!AddIoTimeout(iFd, ulSeq, conn_iter->second, 1.5))");
		DestroyConnect(mapFdAttr.find(iFd));
		return(nullptr);
	}
	if (!AddIoReadEvent(pConn))
	{
		LOG4_ERROR("if (!AddIoReadEvent(conn_iter))");
		DestroyConnect(mapFdAttr.find(iFd));
		return(nullptr);
	}
	if (!AddIoWriteEvent(pConn))
	{
		LOG4_ERROR("if (!AddIoWriteEvent(conn_iter))");
		DestroyConnect(mapFdAttr.find(iFd));
		return(nullptr);
	}
	return pConn;
}

tagConnectionAttr* Worker::CreateAcceptFdAttr(int iFd, uint32 ulSeq,util::E_CODEC_TYPE eCodecType)
{
	x_sock_set_block(iFd, 0);
	tagConnectionAttr* pConn = CreateFdAttr(iFd, ulSeq,eCodecType);
	if (!pConn)
	{
		LOG4_ERROR("CreateFdAttr error");
		close(iFd); // 没有足够资源分配给新连接，直接close掉
		return(nullptr);
	}
	if (boAcceptTimeoutCheck)
	{
		// 为了防止大量连接攻击，初始化连接短时间即超时检查，在正常发送第一个数据包之后才采用正常配置的网络IO超时检查
		if(!AddIoTimeout(iFd, ulSeq,pConn))
		{
			LOG4_ERROR("if(!AddIoTimeout(iFd, ulSeq, pConn, 1.0))");
			DestroyConnect(mapFdAttr.find(iFd));
			return(nullptr);
		}
	}
	if (!AddIoReadEvent(pConn))
	{
		LOG4_ERROR("if (!AddIoReadEvent(iter))");
		DestroyConnect(mapFdAttr.find(iFd));
		return(nullptr);
	}
    if (eCodecType == util::CODEC_HTTPS)
    {
        auto codec_iter = mapCodec.find(util::CODEC_HTTPS);
        if (codec_iter != mapCodec.end())
        {
            static_cast<HttpsCodec*>(codec_iter->second.get())->SetConnectionRole(iFd, true);
        }
    }
	return(pConn);
}


tagConnectionAttr* Worker::CreateFdAttr(int iFd, uint32 ulSeq, util::E_CODEC_TYPE eCodecType)
{
    LOG4_TRACE("%s(fd[%d], seq[%u], codec[%d])", __FUNCTION__, iFd, ulSeq, eCodecType);
    auto fd_attr_iter = mapFdAttr.find(iFd);
    if (fd_attr_iter == mapFdAttr.end())
    {
        auto pConnAttr = std::make_unique<tagConnectionAttr>();
        pConnAttr->iFd = iFd;
        pConnAttr->pRecvBuff = std::make_unique<util::CBuffer>();
        pConnAttr->pSendBuff = std::make_unique<util::CBuffer>();
        pConnAttr->pWaitForSendBuff = std::make_unique<util::CBuffer>();
        pConnAttr->pClientData = std::make_unique<util::CBuffer>();
        pConnAttr->dActiveTime = ev_now(m_loop);
        pConnAttr->ulSeq = ulSeq;
        pConnAttr->eCodecType = eCodecType;
        tagConnectionAttr* pRaw = pConnAttr.get();
        mapFdAttr.insert(std::make_pair(iFd, std::move(pConnAttr)));
        return(pRaw);
    }
    else
    {
        LOG4_ERROR("fd %d is exist!", iFd);
        return(nullptr);
    }
}

tagConnectionAttr* Worker::CreateHttpFdAttr(int iFd, uint32 ulSeq,const std::string& strHost, util::E_CODEC_TYPE eCodecType)
{
	tagConnectionAttr* pConnAttr = CreateFdAttr(iFd, ulSeq, eCodecType);
	if (!pConnAttr)// 没有足够资源分配给新连接，直接close掉
	{
		close(iFd);
		return(nullptr);
	}
	snprintf(pConnAttr->szRemoteAddr, 32, "%s", strHost.c_str());
	if(!AddIoTimeout(iFd, ulSeq, pConnAttr, 2.5))
	{
		LOG4_ERROR("if(AddIoTimeout(stMsgShell.iFd, stMsgShell.ulSeq, conn_iter->second, 2.5)) else");
		DestroyConnect(mapFdAttr.find(iFd));
		return(nullptr);
	}
	pConnAttr->dKeepAlive = 10;
	LOG4_TRACE("set dKeepAlive(%lf)",pConnAttr->dKeepAlive);
	if (!AddIoReadEvent(pConnAttr))
	{
		LOG4_ERROR("if (!AddIoReadEvent(conn_iter))");
		DestroyConnect(mapFdAttr.find(iFd));
		return(nullptr);
	}
	if (!AddIoWriteEvent(pConnAttr))
	{
		LOG4_ERROR("if (!AddIoWriteEvent(conn_iter))");
		DestroyConnect(mapFdAttr.find(iFd));
		return(nullptr);
	}
    if (eCodecType == util::CODEC_HTTPS)
    {
        auto codec_iter = mapCodec.find(util::CODEC_HTTPS);
        if (codec_iter != mapCodec.end())
        {
            static_cast<HttpsCodec*>(codec_iter->second.get())->SetConnectionRole(iFd, false);
        }
    }
	return pConnAttr;
}

tagConnectionAttr* Worker::CreateManagerFdAttr(int iFd,uint32 ulSeq)
{
	tagConnectionAttr* pConn = CreateFdAttr(iFd, ulSeq);
	if (!pConn)
	{
		LOG4_ERROR("!CreateFdAttr");
		return(nullptr);
	}
	if (!AddIoReadEvent(pConn))
	{
		LOG4_ERROR("if (!AddIoReadEvent(conn_iter))");
		DestroyConnect(mapFdAttr.find(iFd));
		return(pConn);
	}
	AddInnerFd(tagMsgShell(iFd,ulSeq));
	return pConn;
}


bool Worker::DestroyConnect(std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>>::iterator iter, bool bMsgShellNotice)
{
    if (iter == mapFdAttr.end())
    {
        return(false);
    }
    tagConnectionAttr* pConn = iter->second.get();
    if (pConn->iFd == iManagerControlFd || pConn->iFd == iManagerDataFd)//收到父进程通信fd关闭
    {
    	LOG4_TRACE("refuse DestroyConnect on manager ipc fd(%u) control(%u) data(%u)",
    			pConn->iFd,iManagerControlFd,iManagerDataFd);
    	return false;
    }
    if (pConn->eCodecType == util::CODEC_HTTPS)
    {
        auto codec_iter = mapCodec.find(util::CODEC_HTTPS);
        if (codec_iter != mapCodec.end())
        {
            static_cast<HttpsCodec*>(codec_iter->second.get())->RemoveConnection(pConn->iFd);
        }
    }
    LOG4_TRACE("%s disconnect, identify %s", pConn->szRemoteAddr, pConn->strIdentify.c_str());
    tagMsgShell stMsgShell(pConn->iFd,pConn->ulSeq);
    DelInnerFd(stMsgShell);
    auto http_step_iter = mapHttpAttr.find(stMsgShell.iFd);
    if (http_step_iter != mapHttpAttr.end())
    {
        mapHttpAttr.erase(http_step_iter);
    }
    DelMsgShell(pConn->strIdentify,stMsgShell);
    if (bMsgShellNotice)
    {
        MsgShellNotice(pConn);
    }
    // Stop libev watchers before close(): if the fd is reused immediately, pending/old watchers corrupt the loop (Manager does DelEvent before close).
    if (pConn->pIoWatcher != nullptr)
    {
        DelEvent(pConn->pIoWatcher, (tagIoWatcherData*)pConn->pIoWatcher->data);
    }
	if (pConn->pTimeWatcher != nullptr)//移除io定时器（即刻回收连接资源）
	{
		LOG4_TRACE("%s() timer ev_timer_stop",__FUNCTION__);
		DelEvent(pConn->pTimeWatcher,(tagIoWatcherData*)pConn->pTimeWatcher->data);
	}
    int iResult = close(iter->first);
    if (0 != iResult)
    {
        if (EINTR != errno)
        {
            LOG4_ERROR("close(%d) failed, result %d and errno %d", pConn->iFd, iResult, errno);
        }
        else
        {
            LOG4_TRACE("close(%d) failed, result %d and errno %d", pConn->iFd, iResult, errno);
        }
    }
    mapFdAttr.erase(iter);
    return(true);
}

void Worker::MsgShellNotice(const tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto cmd_iter = mapSo.find(CMD_REQ_DISCONNECT);
    if (cmd_iter != mapSo.end())
    {
        MsgHead oMsgHead;
        MsgBody oMsgBody;
        oMsgBody.set_body(pConn->strIdentify);
        if (pConn->pClientData != nullptr && pConn->pClientData->ReadableBytes() > 0)
        {
			oMsgBody.set_additional(pConn->pClientData->GetRawReadBuffer(), pConn->pClientData->ReadableBytes());
			user_basic basicinfo;//角色数据包
			if (!basicinfo.ParseFromString(oMsgBody.additional()))
			{
				LOG4_ERROR("user_basic ParseFromString error");
			}
			else
			{
				LOG4_TRACE("%s() basicinfo(%s)", __FUNCTION__,basicinfo.DebugString().c_str());
				if (basicinfo.user_id() > 0)
				{
					oMsgBody.set_targetid(std::to_string(basicinfo.user_id()));
				}
			}
        }
        oMsgHead.set_cmd(CMD_REQ_DISCONNECT);
        oMsgHead.set_seq(GetSequence());
        oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
        cmd_iter->second->pCmd->AnyMessage(tagMsgShell(pConn->iFd,pConn->ulSeq), oMsgHead, oMsgBody);
    }
}
/**
 * @brief 收到完整数据包后处理
 * @note 框架层接收并解析到完整的数据包后调用此函数做数据处理。数据处理可能有多重不同情况出现：
 * 1. 处理成功，仍需继续解析数据包；
 * 2. 处理成功，但无需继续解析数据包；
 * 3. 处理失败，仍需继续解析数据包；
 * 4. 处理失败，但无需继续解析数据包。
 * 是否需要退出解析数据包循环体取决于Dispose()的返回值。返回true就应继续解析数据包，返回
 * false则应退出解析数据包循环体。处理过程或处理完成后，如需回复对端，则直接使用pSendBuff回复数据即可。
 * @param[in] stMsgShell 数据包来源消息外壳
 * @param[in] oInMsgHead 接收的数据包头
 * @param[in] oInMsgBody 接收的数据包体
 * @param[out] oOutMsgHead 待发送的数据包头
 * @param[out] oOutMsgBody 待发送的数据包体
 * @return 是否继续解析数据包（注意不是处理结果）
 */
bool Worker::Dispose(const tagConnectionAttr* pConn,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,MsgHead& oOutMsgHead, MsgBody& oOutMsgBody)
{
    LOG4_TRACE("%s(cmd %u, seq %u)",__FUNCTION__, oInMsgHead.cmd(), oInMsgHead.seq());
    tagMsgShell stMsgShell(pConn->iFd,pConn->ulSeq);
    oOutMsgHead.Clear();
    oOutMsgBody.Clear();
    if (gc_uiCmdReq & oInMsgHead.cmd())    // 请求
    {
    	uint32 uiCmd = gc_uiCmdBit & oInMsgHead.cmd();
        auto cmd_iter = mapSysCmd.find(uiCmd);
        if (cmd_iter != mapSysCmd.end())
        {
            cmd_iter->second->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
            return(true);
        }
		auto cmd_so_iter = mapSo.find(uiCmd);
		if (cmd_so_iter != mapSo.end())
		{
			cmd_so_iter->second->pCmd->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
			return(true);
		}
		if (util::CODEC_PB_INTERNAL == pConn->eCodecType)   // inner_iter != m_mapInnerFd.end() 内部服务往客户端发送  if (std::string("0.0.0.0") == strFromIp)
		{
			cmd_so_iter = mapSo.find(CMD_REQ_TO_CLIENT);
			if (cmd_so_iter != mapSo.end())
			{
				cmd_so_iter->second->pCmd->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
				return(true);
			}
		}
		cmd_so_iter = mapSo.find(CMD_REQ_FROM_CLIENT);
		if (cmd_so_iter != mapSo.end())
		{
			cmd_so_iter->second->pCmd->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
			return(true);
		}
		snprintf(m_pErrBuff, gc_iErrBuffLen, "no handler to dispose cmd %u!", oInMsgHead.cmd());
		LOG4_ERROR("%s", m_pErrBuff);
		OrdinaryResponse oRes;
		oRes.set_err_no(ERR_UNKNOWN_CMD);
		oRes.set_err_msg(m_pErrBuff);
		oOutMsgHead.set_cmd(CMD_RSP_SYS_ERROR);
		oOutMsgHead.set_seq(oInMsgHead.seq());
		if(!BuildMsgBody(oOutMsgHead,oOutMsgBody,oRes))
		{
			LOG4_ERROR("failed to BuildMsgBody as CMD_RSP_SYS_ERROR response,seq(%u)",oInMsgHead.seq());
		}
    }
    else    // 响应
    {
		if (util::CODEC_PB_INTERNAL != pConn->eCodecType)   // inner_iter == m_mapInnerFd.end() 客户端发送到内部服务的响应
		{
			LOG4_TRACE("%s(cmd %u, seq %u)",__FUNCTION__, oInMsgHead.cmd(), oInMsgHead.seq());
			auto cmd_so_iter = mapSo.find(CMD_RSP_TO_CLIENT);//推送客户端的响应转发
			if (cmd_so_iter != mapSo.end())
			{
				cmd_so_iter->second->pCmd->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
				return(true);
			}
		}
		uint32 uiCmd = gc_uiCmdBit & oInMsgHead.cmd();
		auto step_iter = mapCallbackStep.find(oInMsgHead.seq());// 有步骤的优先步骤回调
		if (step_iter != mapCallbackStep.end())
		{
			LOG4_TRACE("receive message, cmd = %u",uiCmd);
			Step* pStep = step_iter->second.get();
			pStep->SetActiveTime(ev_now(m_loop));
			LOG4_TRACE("cmd %u, seq %u, step_seq %u, step addr %p, active_time %lf",
							uiCmd, oInMsgHead.seq(), pStep->GetSequence(),pStep, pStep->GetActiveTime());
			E_CMD_STATUS eResult = pStep->Callback(stMsgShell, oInMsgHead, oInMsgBody);
			if (eResult != STATUS_CMD_RUNNING)
			{
				DeleteCallback(pStep);
			}
			return(true);
		}
		auto cmd_iter = mapSysCmd.find(uiCmd);
		if (cmd_iter != mapSysCmd.end())
		{
			cmd_iter->second->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
			return(true);
		}
		auto cmd_so_iter = mapSo.find(uiCmd);//逻辑指令处理无状态响应(有该指令的则需要配置)
		if (cmd_so_iter != mapSo.end())
		{
			cmd_so_iter->second->pCmd->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
			return(true);
		}
		cmd_so_iter = mapSo.find(CMD_REQ_FROM_CLIENT);
		if (cmd_so_iter != mapSo.end() && GetNodeType() == ROUTER_NODE && uiCmd > 1000)//路由节点处理响应
		{
			cmd_so_iter->second->pCmd->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
			return(true);
		}
		snprintf(m_pErrBuff, gc_iErrBuffLen, "no callback or the callback for seq %u had been timeout! and no cmd(%u) for Dispose", oInMsgHead.seq(),uiCmd);
		LOG4_DEBUG("%s", m_pErrBuff);
    }
    return(true);
}
/**
 * @brief 收到完整的hhtp包后处理
 * @param stMsgShell 数据包来源消息外壳
 * @param oInHttpMsg 接收的HTTP包
 * @param oOutHttpMsg 待发送的HTTP包
 * @return 是否继续解析数据包（注意不是处理结果）
 */
bool Worker::Dispose(const tagConnectionAttr* pConn,const HttpMsg& oInHttpMsg, HttpMsg& oOutHttpMsg)
{
    LOG4_TRACE("%s() oInHttpMsg.type() = %d, oInHttpMsg.path() = %s",__FUNCTION__, oInHttpMsg.type(), oInHttpMsg.path().c_str());
    tagMsgShell stMsgShell(pConn->iFd,pConn->ulSeq);
    oOutHttpMsg.Clear();
    if (HTTP_REQUEST == oInHttpMsg.type())    // 新请求
    {
        auto module_iter = mapModule.find(oInHttpMsg.path());
        if (module_iter == mapModule.end())
        {
            module_iter = mapModule.find("/util/switch");//默认转发路径
        }
        if (module_iter == mapModule.end())
		{
			snprintf(m_pErrBuff, gc_iErrBuffLen, "no module to dispose %s!", oInHttpMsg.path().c_str());
			LOG4_ERROR("%s", m_pErrBuff);
			oOutHttpMsg.set_type(HTTP_RESPONSE);
			oOutHttpMsg.set_status_code(404);
			oOutHttpMsg.set_http_major(oInHttpMsg.http_major());
			oOutHttpMsg.set_http_minor(oInHttpMsg.http_minor());
		}
		else
		{
			module_iter->second->pModule->AnyMessage(stMsgShell, oInHttpMsg);
		}
    }
    else
    {
        auto http_step_iter = mapHttpAttr.find(stMsgShell.iFd);
        if (http_step_iter == mapHttpAttr.end())
        {
            LOG4_ERROR("no callback for http response from %s!", oInHttpMsg.url().c_str());
        }
        else
        {
            if (http_step_iter->second.begin() != http_step_iter->second.end())
            {
                auto step_iter = mapCallbackStep.find(*http_step_iter->second.begin());
                if (step_iter != mapCallbackStep.end())   // 步骤回调
                {
                    step_iter->second->SetActiveTime(ev_now(m_loop));
                    E_CMD_STATUS eResult = ((HttpStep*)step_iter->second.get())->Callback(stMsgShell, oInHttpMsg);
                    if (eResult != STATUS_CMD_RUNNING)
                    {
                        DeleteCallback(step_iter->second.get());
                    }
                }
                else
                {
                    snprintf(m_pErrBuff, gc_iErrBuffLen, "no callback or the callback for seq %u had been timeout!",
                                    *http_step_iter->second.begin());
                    LOG4_WARN("%s", m_pErrBuff);
                }
                http_step_iter->second.erase(http_step_iter->second.begin());
            }
            else
            {
                LOG4_ERROR("no callback for http response from %s!", oInHttpMsg.url().c_str());
            }
        }
    }
    return(true);
}

bool Worker::Dispose(util::MysqlAsyncConn *c, util::SqlTask *task, MYSQL_RES *pResultSet)
{
	auto step_iter = mapCallbackStep.find(((CustomMysqlHandler*)task->handler.get())->m_uiMysqlStepSeq);
	if (step_iter != mapCallbackStep.end() && step_iter->second != nullptr)   // 步骤回调
	{
		E_CMD_STATUS eResult;
		step_iter->second->SetActiveTime(ev_now(m_loop));
		eResult = ((MysqlStep*)step_iter->second.get())->Callback(c,task,pResultSet);
		if (eResult != STATUS_CMD_RUNNING)
		{
			DeleteCallback(step_iter->second.get());
		}
	}
	else
	{
		snprintf(m_pErrBuff, gc_iErrBuffLen, "no callback or the callback for MysqlStepSeq %u had been timeout!",
				((CustomMysqlHandler*)task->handler.get())->m_uiMysqlStepSeq);
		LOG4_WARN("%s", m_pErrBuff);
	}
	return(true);
}

} /* namespace net */
