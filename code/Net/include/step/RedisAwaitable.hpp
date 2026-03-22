/*******************************************************************************
 * Project:  Net
 * @file     RedisAwaitable.hpp
 * @brief    Redis 请求的 Awaitable 类型
 * @author   cjy
 * @date:    2024
 * @note     用于在协程中 co_await Redis 请求
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_REDISAWAITABLE_HPP_
#define SRC_STEP_REDISAWAITABLE_HPP_

#include <coroutine>
#include <string>
#include <vector>
#include <memory>
#include "Awaitable.hpp"

namespace net
{

class StepCo20;
class RedisStepBridge;

/**
 * @brief Redis 命令类型枚举
 */
enum class RedisCommandType
{
    GET,
    SET,
    DEL,
    HGET,
    HSET,
    HDEL,
    LPUSH,
    RPUSH,
    LPOP,
    RPOP,
    LRANGE,
    EXPIRE,
    TTL,
    EXISTS,
    INCR,
    DECR,
    CUSTOM  // 自定义命令
};

/**
 * @brief Redis 响应数据
 */
struct RedisReply
{
    enum class Type
    {
        NIL,
        STRING,
        INTEGER,
        ARRAY,
        ERROR
    };
    
    Type type = Type::NIL;
    std::string str;
    int64_t integer = 0;
    std::vector<RedisReply> elements;
    int errNo = 0;
    std::string errMsg;
    
    bool IsOk() const { return type != Type::ERROR && errNo == 0; }
    bool IsNil() const { return type == Type::NIL; }
    
    // 便捷方法
    std::string AsString() const { return str; }
    int64_t AsInteger() const { return integer; }
    const std::vector<RedisReply>& AsArray() const { return elements; }
};

/**
 * @brief Redis 请求的 Awaitable 类型
 * @note 封装 Redis 命令，可在协程中使用 co_await
 */
class RedisAwaitable
{
    friend class RedisStepBridge;

public:
    /**
     * @brief 构造函数
     * @param pState 所属的 StepCo20
     * @param strHost Redis 服务器地址
     * @param iPort Redis 端口
     * @param strCommand Redis 命令
     */
    RedisAwaitable(StepCo20* pState, 
                   const std::string& strHost,
                   int iPort,
                   const std::string& strCommand)
        : m_pState(pState)
        , m_strHost(strHost)
        , m_iPort(iPort)
        , m_strCommand(strCommand)
        , m_cmdType(RedisCommandType::CUSTOM)
    {
    }
    
    /**
     * @brief 构造函数（带参数列表）
     * @param pState 所属的 StepCo20
     * @param strHost Redis 服务器地址
     * @param iPort Redis 端口
     * @param cmdType 命令类型
     * @param args 命令参数
     */
    RedisAwaitable(StepCo20* pState,
                   const std::string& strHost,
                   int iPort,
                   RedisCommandType cmdType,
                   std::vector<std::string> args)
        : m_pState(pState)
        , m_strHost(strHost)
        , m_iPort(iPort)
        , m_cmdType(cmdType)
        , m_args(std::move(args))
    {
        BuildCommand();
    }

    /**
     * @brief 检查是否准备好（总是返回 false，需要挂起）
     */
    bool await_ready() const noexcept
    {
        return false;
    }

    /**
     * @brief 挂起时的处理
     * @param h 协程句柄
     * @note 保存句柄，发起 Redis 请求
     */
    void await_suspend(std::coroutine_handle<> h);

    /**
     * @brief 恢复时的处理
     * @return Redis 响应
     */
    RedisReply await_resume();

    /**
     * @brief 获取命令类型
     */
    RedisCommandType GetCommandType() const { return m_cmdType; }

    /**
     * @brief 获取完整命令
     */
    const std::string& GetCommand() const { return m_strCommand; }

    /**
     * @brief 获取主机地址
     */
    const std::string& GetHost() const { return m_strHost; }

    /**
     * @brief 获取端口
     */
    int GetPort() const { return m_iPort; }

private:
    /**
     * @brief 根据命令类型和参数构建命令字符串
     */
    void BuildCommand()
    {
        switch (m_cmdType)
        {
        case RedisCommandType::GET:
            if (m_args.size() >= 1)
                m_strCommand = "GET " + m_args[0];
            break;
        case RedisCommandType::SET:
            if (m_args.size() >= 2)
                m_strCommand = "SET " + m_args[0] + " " + m_args[1];
            break;
        case RedisCommandType::DEL:
            if (m_args.size() >= 1)
                m_strCommand = "DEL " + m_args[0];
            break;
        case RedisCommandType::HGET:
            if (m_args.size() >= 2)
                m_strCommand = "HGET " + m_args[0] + " " + m_args[1];
            break;
        case RedisCommandType::HSET:
            if (m_args.size() >= 3)
                m_strCommand = "HSET " + m_args[0] + " " + m_args[1] + " " + m_args[2];
            break;
        case RedisCommandType::EXPIRE:
            if (m_args.size() >= 2)
                m_strCommand = "EXPIRE " + m_args[0] + " " + m_args[1];
            break;
        case RedisCommandType::TTL:
            if (m_args.size() >= 1)
                m_strCommand = "TTL " + m_args[0];
            break;
        case RedisCommandType::EXISTS:
            if (m_args.size() >= 1)
                m_strCommand = "EXISTS " + m_args[0];
            break;
        case RedisCommandType::INCR:
            if (m_args.size() >= 1)
                m_strCommand = "INCR " + m_args[0];
            break;
        case RedisCommandType::DECR:
            if (m_args.size() >= 1)
                m_strCommand = "DECR " + m_args[0];
            break;
        default:
            // CUSTOM 类型，使用传入的命令
            break;
        }
    }

    StepCo20* m_pState;
    std::string m_strHost;
    int m_iPort;
    std::string m_strCommand;
    RedisCommandType m_cmdType;
    std::vector<std::string> m_args;
    RedisReply m_reply;
};

/**
 * @brief Redis 操作辅助类
 * @note 提供便捷的 Redis 协程操作方法
 */
class RedisCoHelper
{
public:
    explicit RedisCoHelper(StepCo20* pState, 
                           const std::string& strHost = "127.0.0.1",
                           int iPort = 6379)
        : m_pState(pState)
        , m_strHost(strHost)
        , m_iPort(iPort)
    {
    }
    
    /**
     * @brief GET 命令
     */
    RedisAwaitable Get(const std::string& key)
    {
        return RedisAwaitable(m_pState, m_strHost, m_iPort, 
                              RedisCommandType::GET, {key});
    }
    
    /**
     * @brief SET 命令
     */
    RedisAwaitable Set(const std::string& key, const std::string& value)
    {
        return RedisAwaitable(m_pState, m_strHost, m_iPort,
                              RedisCommandType::SET, {key, value});
    }
    
    /**
     * @brief DEL 命令
     */
    RedisAwaitable Del(const std::string& key)
    {
        return RedisAwaitable(m_pState, m_strHost, m_iPort,
                              RedisCommandType::DEL, {key});
    }
    
    /**
     * @brief HGET 命令
     */
    RedisAwaitable HGet(const std::string& key, const std::string& field)
    {
        return RedisAwaitable(m_pState, m_strHost, m_iPort,
                              RedisCommandType::HGET, {key, field});
    }
    
    /**
     * @brief HSET 命令
     */
    RedisAwaitable HSet(const std::string& key, const std::string& field, 
                        const std::string& value)
    {
        return RedisAwaitable(m_pState, m_strHost, m_iPort,
                              RedisCommandType::HSET, {key, field, value});
    }
    
    /**
     * @brief EXPIRE 命令
     */
    RedisAwaitable Expire(const std::string& key, int seconds)
    {
        return RedisAwaitable(m_pState, m_strHost, m_iPort,
                              RedisCommandType::EXPIRE, {key, std::to_string(seconds)});
    }
    
    /**
     * @brief 自定义命令
     */
    RedisAwaitable Execute(const std::string& command)
    {
        return RedisAwaitable(m_pState, m_strHost, m_iPort, command);
    }
    
private:
    StepCo20* m_pState;
    std::string m_strHost;
    int m_iPort;
};

} /* namespace net */

#endif /* SRC_STEP_REDISAWAITABLE_HPP_ */
