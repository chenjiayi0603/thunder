/*******************************************************************************
 * Project:  Net
 * @file     RedisStep.hpp
 * @brief    带Redis的异步步骤基类
 * @author   cjy
 * @date:    2019年8月15日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_REDISSTEP_HPP_
#define SRC_STEP_REDISSTEP_HPP_
#include <memory>
#include <list>
#include "dbi/redis/RedisCmd.hpp"
#include "Step.hpp"

namespace net
{

/**
 * @brief redis异步访问步骤
 * @note 回调后一定会被删除
 */
class RedisStep: public Step
{
public:
    RedisStep(Step* pNextStep = nullptr);
    RedisStep(const tagMsgShell& stReqMsgShell, const MsgHead& oReqMsgHead, const MsgBody& oReqMsgBody, Step* pNextStep = nullptr);
    virtual ~RedisStep();
    /**
     * @brief redis步骤回调
     * @param c redis连接上下文
     * @param status 回调状态
     * @param pReply 执行结果集
     */
    virtual E_CMD_STATUS Callback(const redisAsyncContext *c,int status,redisReply* pReply) = 0;

    RedisStep(const RedisStep &step) = delete;
    RedisStep& operator = (const RedisStep &step) = delete;
public:
    /**
     * @brief 从Step派生的回调函数
     * @deprecated RedisStep的Callback与普通Step的Callback不一样
     * @note 从Step派生的回调函数在reids回调中不需要，所以直接返回
     */
    virtual E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,void* data = nullptr)override
    {
        return(STATUS_CMD_COMPLETED);
    }
    /**
     * @brief 超时回调
     * @note redis step 暂时不启用超时机制
     * @return 回调状态
     */
    virtual E_CMD_STATUS Timeout()override{return(STATUS_CMD_FAULT);}
    void AddSendCounter(){m_uiSendCounter++;}
    void AddCallBackCounter(){m_uiCallBackCounter++;}
    uint32 m_uiSendCounter = 0;
    uint32 m_uiCallBackCounter = 0;
public:
    util::RedisCmd* RedisCmd(){return(m_pRedisCmd);}
    const util::RedisCmd* GetRedisCmd(){return(m_pRedisCmd);}
private:
    util::RedisCmd* m_pRedisCmd = nullptr;
};


/**
 * @brief Redis连接属性
 * @note  Redis连接属性，因内部带有许多指针，并且没有必要提供深拷贝构造，所以不可以拷贝，也无需拷贝
 */
struct tagRedisAttr
{
    uint32 ulSeq = 0;                       ///< redis连接序列号
    redisReply* pReply = nullptr;           ///< redis命令执行结果
    bool bIsReady = false;                  ///< redis连接是否准备就绪
    bool bIsAuthFail = false;
    std::list<std::unique_ptr<RedisStep>> listData;         ///< redis连接回调数据
    std::list<std::unique_ptr<RedisStep>> listWaitData;     ///< redis等待连接成功需执行命令的数据

    std::string strPassword;

    tagRedisAttr() = default;
};

} /* namespace net */

#endif /* SRC_STEP_REDISSTEP_HPP_ */
