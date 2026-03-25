#include "coro/MySqlAwaitable.hpp"
#include "coro/StepCo20.hpp"
#include "labor/Labor.hpp"
#include "step/MysqlStep.hpp"

namespace net
{

class MySqlStepBridge : public MysqlStep
{
public:
    MySqlStepBridge(MySqlAwaitable* pAwaitable,
                    std::coroutine_handle<> h,
                    const util::tagDbConnInfo& dbConn)
        : MysqlStep(dbConn)
        , m_pAwaitable(pAwaitable)
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

    E_CMD_STATUS Callback(util::MysqlAsyncConn* c, util::SqlTask* task, MYSQL_RES* pResultSet) override
    {
        if (m_pAwaitable != nullptr && task != nullptr)
        {
            MySqlReply& out = m_pAwaitable->m_reply;
            out = MySqlReply{};
            if (task->iErrno != 0)
            {
                out.errNo = task->iErrno;
                out.errMsg = task->errmsg;
            }
            else
            {
                out.errNo = 0;
                if (pResultSet != nullptr && c != nullptr)
                {
                    m_pMysqlResSet->Init(pResultSet, c->GetMysql());
                    util::T_vecResultSet vec;
                    (void)m_pMysqlResSet->GetResultSet(vec);
                    out.result = std::make_unique<util::T_vecResultSet>(std::move(vec));
                }
            }
        }
        if (m_handle && !m_handle.done())
        {
            m_handle.resume();
        }
        return STATUS_CMD_COMPLETED;
    }

    E_CMD_STATUS Timeout() override
    {
        ++m_uiTimeOutCounter;
        if (m_uiTimeOutCounter < m_uiTimeOutMax)
        {
            return STATUS_CMD_RUNNING;
        }

        if (m_pAwaitable != nullptr)
        {
            m_pAwaitable->m_reply = MySqlReply{};
            m_pAwaitable->m_reply.errNo = -1;
            m_pAwaitable->m_reply.errMsg = "mysql operation timeout";
        }
        if (m_handle && !m_handle.done())
        {
            m_handle.resume();
        }
        return STATUS_CMD_FAULT;
    }

private:
    MySqlAwaitable* m_pAwaitable = nullptr;
    std::coroutine_handle<> m_handle;
};

MySqlAwaitable::MySqlAwaitable(StepCo20* pState,
                               const util::tagDbConnInfo& dbConn,
                               std::string strSql,
                               std::uint8_t uiCmdType,
                               std::uint32_t uiTimeOutMax,
                               std::uint8_t uiTimeOutRetry,
                               double dTimeout)
    : m_pState(pState)
    , m_dbConn(dbConn)
    , m_strSql(std::move(strSql))
    , m_uiCmdType(uiCmdType)
    , m_uiTimeOutMax(uiTimeOutMax)
    , m_uiTimeOutRetry(uiTimeOutRetry)
    , m_dTimeout(dTimeout)
{
}

void MySqlAwaitable::await_suspend(std::coroutine_handle<> h)
{
    if (m_pState == nullptr)
    {
        m_reply = MySqlReply{};
        m_reply.errNo = -1;
        m_reply.errMsg = "null StepCo20";
        if (h && !h.done())
        {
            h.resume();
        }
        return;
    }
    if (m_strSql.empty())
    {
        m_reply = MySqlReply{};
        m_reply.errNo = -3;
        m_reply.errMsg = "empty mysql sql";
        if (h && !h.done())
        {
            h.resume();
        }
        return;
    }

    m_reply = MySqlReply{};
    auto* pBridge = new MySqlStepBridge(this, h, m_dbConn);
    pBridge->Init(m_uiTimeOutMax, m_uiTimeOutRetry);
    pBridge->SetTask(m_strSql, m_uiCmdType);

    if (!GetLabor()->RegisterCallback(pBridge, m_dTimeout))
    {
        delete pBridge;
        m_reply.errNo = -2;
        m_reply.errMsg = "RegisterCallback(Step*) failed";
        if (h && !h.done())
        {
            h.resume();
        }
        return;
    }

    if (!GetLabor()->RegisterCallback(static_cast<MysqlStep*>(pBridge)))
    {
        GetLabor()->DeleteCallback(pBridge);
        m_reply.errNo = -2;
        m_reply.errMsg = "RegisterCallback(MysqlStep*) failed";
        if (h && !h.done())
        {
            h.resume();
        }
        return;
    }
}

MySqlReply MySqlAwaitable::await_resume()
{
    return std::move(m_reply);
}

} // namespace net
