/*******************************************************************************
 * Project:  Net
 * @file     MySqlAwaitable.hpp
 * @brief    MySQL 异步查询/执行的 Awaitable，供 StepCo20 协程体内 co_await
 * @note     通过内部 MySqlStepBridge（MysqlStep）走 Worker::Dispose 回调路径；
 *           结果集深拷贝为 std::unique_ptr<util::T_vecResultSet>，便于跨线程只读使用。
 ******************************************************************************/
#ifndef SRC_CORO_MYSQLAWAITABLE_HPP_
#define SRC_CORO_MYSQLAWAITABLE_HPP_

#include <coroutine>
#include <cstdint>
#include <memory>
#include <string>

#include "dbi/Dbi.hpp"
#include "dbi/MysqlAsyncConn.h"

namespace net
{

class StepCo20;
class MySqlStepBridge;

/**
 * @brief MySQL 协程 await 返回结构
 */
struct MySqlReply
{
    int errNo = 0;
    std::string errMsg;
    /// SELECT 成功时非空；INSERT/UPDATE/DELETE 成功时通常为空
    std::unique_ptr<util::T_vecResultSet> result;

    bool IsOk() const noexcept { return errNo == 0; }
};

/**
 * @brief 在协程中 co_await 等待一次 MySQL 异步操作完成
 */
class MySqlAwaitable
{
    friend class MySqlStepBridge;

public:
    static constexpr double kDefaultTimeoutSeconds = 300.0;

    MySqlAwaitable(StepCo20* pState,
                   const util::tagDbConnInfo& dbConn,
                   std::string strSql,
                   std::uint8_t uiCmdType,
                   std::uint32_t uiTimeOutMax = 3,
                   std::uint8_t uiTimeOutRetry = 0,
                   double dTimeout = 0.0);

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h);

    MySqlReply await_resume();

    const std::string& GetSql() const { return m_strSql; }
    std::uint8_t GetCmdType() const { return m_uiCmdType; }

private:
    StepCo20* m_pState = nullptr;
    util::tagDbConnInfo m_dbConn;
    std::string m_strSql;
    std::uint8_t m_uiCmdType = 0;
    std::uint32_t m_uiTimeOutMax = 3;
    std::uint8_t m_uiTimeOutRetry = 0;
    double m_dTimeout = kDefaultTimeoutSeconds;

    MySqlReply m_reply;
};

/**
 * @brief 便捷构造 MySqlAwaitable（同一连接上多次查询）
 */
class MySqlCoHelper
{
public:
    MySqlCoHelper(StepCo20* pState, util::tagDbConnInfo dbConn)
        : m_pState(pState)
        , m_dbConn(std::move(dbConn))
    {
    }

    /// SELECT / 需结果集的语句（util::eSqlTaskOper_select）
    MySqlAwaitable Query(const std::string& sql,
                         std::uint32_t uiTimeOutMax = 3,
                         std::uint8_t uiTimeOutRetry = 0,
                         double dTimeout = 0.0)
    {
        return MySqlAwaitable(m_pState, m_dbConn, sql, util::eSqlTaskOper_select, uiTimeOutMax,
                              uiTimeOutRetry, dTimeout);
    }

    /// INSERT/UPDATE/DELETE 等（util::eSqlTaskOper_exec）
    MySqlAwaitable Exec(const std::string& sql,
                        std::uint32_t uiTimeOutMax = 3,
                        std::uint8_t uiTimeOutRetry = 0,
                        double dTimeout = 0.0)
    {
        return MySqlAwaitable(m_pState, m_dbConn, sql, util::eSqlTaskOper_exec, uiTimeOutMax,
                              uiTimeOutRetry, dTimeout);
    }

private:
    StepCo20* m_pState = nullptr;
    util::tagDbConnInfo m_dbConn;
};

} // namespace net

#endif /* SRC_CORO_MYSQLAWAITABLE_HPP_ */
