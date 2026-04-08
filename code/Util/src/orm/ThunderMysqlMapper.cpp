#include "ThunderMysqlMapper.hpp"
#include "dbi/MysqlDbi.hpp"

#include <cctype>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace thunder::orm
{

namespace
{

bool safeIdent(const std::string& s)
{
    if (s.empty())
    {
        return false;
    }
    for (unsigned char c : s)
    {
        if (!(std::isalnum(c) || c == '_'))
        {
            return false;
        }
    }
    return true;
}

/** @return SQL 或空串（err_out 非空表示校验/拼串失败） */
std::string buildInsertSql(util::CMysqlDbi& db,
                           const std::string& table,
                           const std::map<std::string, std::string>& row,
                           std::string& err_out)
{
    err_out.clear();
    if (!safeIdent(table) || row.empty())
    {
        err_out = "invalid table or empty row";
        return {};
    }
    std::ostringstream cols;
    std::ostringstream vals;
    bool first = true;
    for (const auto& kv : row)
    {
        if (!safeIdent(kv.first))
        {
            err_out = std::string("invalid column: ") + kv.first;
            return {};
        }
        const std::string& raw = kv.second;
        std::vector<char> to(raw.size() * 2 + 8);
        const int n = db.EscapeString(to.data(), raw.data(), static_cast<unsigned long>(raw.size()));
        if (n < 0)
        {
            err_out = "EscapeString failed";
            return {};
        }
        if (!first)
        {
            cols << ',';
            vals << ',';
        }
        first = false;
        cols << '`' << kv.first << '`';
        vals << '\'';
        vals << std::string_view(to.data(), static_cast<size_t>(n));
        vals << '\'';
    }
    std::ostringstream sql;
    sql << "INSERT INTO `" << table << "` (" << cols.str() << ") VALUES (" << vals.str() << ')';
    return sql.str();
}

void runInsert(util::tagDbConnInfo conn,
               const std::string& table,
               const std::map<std::string, std::string>& row,
               MysqlInsertOk on_ok,
               MysqlErr on_err)
{
    util::CMysqlDbi db(conn.m_szDbHost,
                       conn.m_szDbUser,
                       conn.m_szDbPwd,
                       conn.m_szDbName,
                       conn.m_szDbCharSet[0] != '\0' ? conn.m_szDbCharSet : "utf8",
                       conn.m_uiDbPort);
    if (db.GetErrno() != 0)
    {
        on_err(db.GetError());
        return;
    }
    std::string build_err;
    std::string sql = buildInsertSql(db, table, row, build_err);
    if (!build_err.empty())
    {
        on_err(build_err);
        return;
    }
    unsigned long long insert_id = 0;
    if (db.ExecSql(sql, insert_id) != 0)
    {
        on_err(db.GetError());
        return;
    }
    on_ok(static_cast<std::uint64_t>(insert_id));
}

} // namespace

MysqlMapper::MysqlMapper(util::tagDbConnInfo conn) : conn_(conn) {}

void MysqlMapper::insert(const std::string& table,
                         const std::map<std::string, std::string>& row,
                         MysqlInsertOk on_ok,
                         MysqlErr on_err)
{
    if (!on_ok || !on_err)
    {
        return;
    }
    if (!safeIdent(table) || row.empty())
    {
        on_err("invalid table or empty row");
        return;
    }
    for (const auto& kv : row)
    {
        if (!safeIdent(kv.first))
        {
            on_err(std::string("invalid column: ") + kv.first);
            return;
        }
    }
    std::thread(runInsert, conn_, table, row, std::move(on_ok), std::move(on_err)).detach();
}

std::future<std::uint64_t> MysqlMapper::insertFuture(const std::string& table,
                                                     const std::map<std::string, std::string>& row)
{
    return std::async(std::launch::async, [conn = conn_, table, row]() -> std::uint64_t {
        util::CMysqlDbi db(conn.m_szDbHost,
                           conn.m_szDbUser,
                           conn.m_szDbPwd,
                           conn.m_szDbName,
                           conn.m_szDbCharSet[0] != '\0' ? conn.m_szDbCharSet : "utf8",
                           conn.m_uiDbPort);
        if (db.GetErrno() != 0)
        {
            throw std::runtime_error(db.GetError());
        }
        std::string build_err;
        std::string sql = buildInsertSql(db, table, row, build_err);
        if (!build_err.empty())
        {
            throw std::runtime_error(build_err);
        }
        unsigned long long insert_id = 0;
        if (db.ExecSql(sql, insert_id) != 0)
        {
            throw std::runtime_error(db.GetError());
        }
        return static_cast<std::uint64_t>(insert_id);
    });
}

} // namespace thunder::orm
