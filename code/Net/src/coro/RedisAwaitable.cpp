#include "coro/RedisAwaitable.hpp"
#include "step/RedisStep.hpp"
#include "labor/Labor.hpp"
#include "hiredis_vip/read.h"
#include <utility>

namespace net
{

namespace
{

void FillRedisReplyFromHiredis(RedisReply& out, redisReply* p)
{
    if (p == nullptr)
    {
        out.type = RedisReply::Type::NIL;
        return;
    }
    switch (p->type)
    {
    case REDIS_REPLY_ERROR:
        out.type = RedisReply::Type::ERROR;
        if (p->str != nullptr && p->len >= 0)
        {
            out.errMsg.assign(p->str, static_cast<size_t>(p->len));
        }
        break;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_STATUS:
        out.type = RedisReply::Type::STRING;
        if (p->str != nullptr && p->len >= 0)
        {
            out.str.assign(p->str, static_cast<size_t>(p->len));
        }
        break;
    case REDIS_REPLY_INTEGER:
        out.type = RedisReply::Type::INTEGER;
        out.integer = p->integer;
        break;
    case REDIS_REPLY_NIL:
        out.type = RedisReply::Type::NIL;
        break;
    case REDIS_REPLY_ARRAY:
        out.type = RedisReply::Type::ARRAY;
        out.elements.clear();
        out.elements.reserve(p->elements);
        for (size_t i = 0; i < p->elements; ++i)
        {
            RedisReply child;
            FillRedisReplyFromHiredis(child, p->element[i]);
            out.elements.push_back(std::move(child));
        }
        break;
    default:
        out.type = RedisReply::Type::STRING;
        if (p->str != nullptr && p->len > 0)
        {
            out.str.assign(p->str, static_cast<size_t>(p->len));
        }
        break;
    }
}

} // namespace

class RedisStepBridge : public RedisStep
{
public:
    RedisStepBridge(RedisAwaitable* pAwaitable, std::coroutine_handle<> h)
        : m_pAwaitable(pAwaitable)
        , m_handle(h)
    {
    }

    E_CMD_STATUS Emit(int iErrno = 0, const std::string& strErrMsg = "",
                      const std::string& strErrShow = "") override
    {
        (void)iErrno;
        (void)strErrMsg;
        (void)strErrShow;
        return STATUS_CMD_RUNNING;
    }

    E_CMD_STATUS Callback(const redisAsyncContext* c, int status, redisReply* pReply) override
    {
        if (m_pAwaitable != nullptr)
        {
            RedisReply& reply = m_pAwaitable->m_reply;
            reply = RedisReply{};
            if (status != REDIS_OK || pReply == nullptr)
            {
                reply.type = RedisReply::Type::ERROR;
                reply.errNo = status;
                if (c != nullptr && c->errstr != nullptr)
                {
                    reply.errMsg = c->errstr;
                }
                else
                {
                    reply.errMsg = "redis error or null reply";
                }
            }
            else
            {
                FillRedisReplyFromHiredis(reply, pReply);
            }
        }
        if (m_handle && !m_handle.done())
        {
            m_handle.resume();
        }
        return STATUS_CMD_COMPLETED;
    }

private:
    RedisAwaitable* m_pAwaitable;
    std::coroutine_handle<> m_handle;
};

void RedisAwaitable::await_suspend(std::coroutine_handle<> h)
{
    if (m_pState == nullptr)
    {
        m_reply = RedisReply{};
        m_reply.type = RedisReply::Type::ERROR;
        m_reply.errNo = -1;
        m_reply.errMsg = "null StepCo20";
        if (h && !h.done())
        {
            h.resume();
        }
        return;
    }
    if (m_strCommand.empty())
    {
        m_reply = RedisReply{};
        m_reply.type = RedisReply::Type::ERROR;
        m_reply.errNo = -3;
        m_reply.errMsg = "empty redis command";
        if (h && !h.done())
        {
            h.resume();
        }
        return;
    }

    m_reply = RedisReply{};
    auto* pBridge = new RedisStepBridge(this, h);
    pBridge->RedisCmd()->AppendRawCmd(m_strCommand);
    if (!GetLabor()->AutoRedisCmd(m_strHost, m_iPort, pBridge))
    {
        delete pBridge;
        m_reply.type = RedisReply::Type::ERROR;
        m_reply.errNo = -2;
        m_reply.errMsg = "AutoRedisCmd failed";
        if (h && !h.done())
        {
            h.resume();
        }
    }
}

RedisReply RedisAwaitable::await_resume()
{
    return std::move(m_reply);
}

} // namespace net
