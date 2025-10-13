/*******************************************************************************
 * Project:  Net
 * @file     Session.hpp
 * @brief    会话基类
 * @author   cjy
 * @date:    2019年7月28日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SESSION_HPP_
#define SESSION_HPP_

#include "libev/ev.h"         // need ev_tstamp
#include "NetDefine.hpp"
#include "NetError.hpp"
#include "labor/Labor.hpp"

namespace net
{

class Worker;

/**
 * @brief 会话对象
 * @note 存储信息，定时调度
 */
class Session
{
public:
    Session(uint64 ulSessionId, ev_tstamp dSessionTimeout = 60.0, const std::string& strSessionClass = "net::Session");
    Session(const std::string& strSessionId, ev_tstamp dSessionTimeout = 60.0, const std::string& strSessionClass = "net::Session");
    virtual ~Session();
    /**
     * @brief 会话超时回调
     */
    virtual E_CMD_STATUS Timeout() = 0;
    const std::string& GetSessionId() const{return(m_strSessionId);}
    const std::string& GetSessionClass() const{return(m_strSessionClassName);}
    void SetPermanent(){m_boPermanent = true;}//设置会话为永久会话（表示会话不会因超时而注销，用于异步发送的参数）
    bool IsPermanent()const{return m_boPermanent;}
    virtual bool Init(const util::CJsonObject &conf){return true;}//需要初始化的则继承
public:
protected:
    /**
     * @brief 设置会话最近刷新时间
     */
    virtual void SetActiveTime(ev_tstamp activeTime){m_activeTime = activeTime;}
    /**
     * @brief 获取会话刷新时间
     */
    ev_tstamp GetActiveTime() const{return(m_activeTime);}
    /**
     * @brief 获取会话刷新时间
     */
    ev_tstamp GetTimeout() const{return(m_dSessionTimeout);}
    /**
     * @brief 是否注册了回调
     * @return 注册结果
     */
    bool IsRegistered() const{return(m_bRegistered);}
private:
    /**
     * @brief 设置为已注册状态
     */
    void SetRegistered(){m_bRegistered = true;}
private:
    ev_tstamp m_dSessionTimeout;
    bool m_bRegistered = false;
    ev_tstamp m_activeTime = 0;
    std::string m_strSessionId;
    std::string m_strSessionClassName;
    ev_timer* m_pTimeoutWatcher = nullptr;
    bool m_boPermanent = false;

    friend class Labor;
    friend class Worker;
    friend class Manager;
};

template <typename T> T* MakeSession(const std::string& strSessionId, const std::string& strSessionClass,ev_tstamp dSessionTimeout=60.0,const util::CJsonObject &conf = util::CJsonObject())
{
	T* pSession = (T*) GetLabor()->GetSession(strSessionId,strSessionClass);
	if (pSession)
	{
		return (pSession);
	}
	pSession = new T(strSessionId,dSessionTimeout,strSessionClass);
	if (pSession == nullptr)
	{
		LOG4_ERROR("error %d: new %s() error!",net::ERR_NEW,strSessionClass.c_str());
		return (nullptr);
	}
	if (GetLabor()->RegisterCallback(pSession))
	{
		if (!pSession->Init(conf))
		{
			GetLabor()->DeleteCallback(pSession);
			return nullptr;
		}
		return (pSession);
	}
	else
	{
		LOG4_ERROR( "register pSession error!");
		SAFE_DELETE(pSession);
	}
	return (nullptr);
}

template <typename T> T* MakeSession(uint64 uiSessionId, const std::string& strSessionClass,ev_tstamp dSessionTimeout=60.0,const util::CJsonObject &conf = util::CJsonObject())
{
	LOG4_INFO("MakeSession uiSessionId %llu: strSessionClass %s!",uiSessionId,strSessionClass.c_str());
	T* pSession = (T*) GetLabor()->GetSession(uiSessionId,strSessionClass);
	if (pSession)
	{
		return (pSession);
	}
	LOG4_INFO("new Session uiSessionId %llu: strSessionClass %s!",uiSessionId,strSessionClass.c_str());
	pSession = new T(uiSessionId,dSessionTimeout,strSessionClass);
	if (pSession == nullptr)
	{
		LOG4_ERROR("error %d: new %s() error!",net::ERR_NEW,strSessionClass.c_str());
		return (nullptr);
	}
	if (GetLabor()->RegisterCallback(pSession))
	{
		if (!pSession->Init(conf))
		{
			GetLabor()->DeleteCallback(pSession);
			return nullptr;
		}
		return (pSession);
	}
	else
	{
		LOG4_ERROR( "register pSession error!");
		SAFE_DELETE(pSession);
	}
	return (nullptr);
}

/**
* @brief 获取全局配置会话
*/
template <typename T> T* GetGlobalConfigSession(const std::string & configFileName = "",ev_tstamp dSessionTimeout=1.0)
{
	static T* g_pSession = nullptr;
	if (g_pSession) return g_pSession;
	util::CJsonObject oCurrentConf;       ///< 当前加载的配置
	if (configFileName.size() != 0)
	{
		if (!net::GetConfig(oCurrentConf,net::GetConfigPath() + configFileName))//"MongoCmd.json"
		{
			LOG4_ERROR("Open conf (%s)!",(net::GetConfigPath() + configFileName).c_str());
			return nullptr;
		}
	}
	return (g_pSession = net::MakeSession<T>(T::SessionClass(),T::SessionClass(),dSessionTimeout,oCurrentConf));
}

} /* namespace net */

#endif /* SESSION_HPP_ */
