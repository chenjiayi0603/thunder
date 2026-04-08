/*******************************************************************************
 * Thunder 轻量 MySQL ORM 风格封装（参考 Drogon Mapper：异步回调 + insertFuture）
 * @note 线程内使用同步 CMysqlDbi，适合工具/测试；在线 Worker 上请继续用 MysqlStep。
 ******************************************************************************/
#ifndef THUNDER_ORM_MYSQL_MAPPER_HPP_
#define THUNDER_ORM_MYSQL_MAPPER_HPP_

#include "dbi/Dbi.hpp"

#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <string>

namespace thunder::orm
{

using MysqlInsertOk = std::function<void(std::uint64_t insert_id)>;
using MysqlErr = std::function<void(const std::string& what)>;

/**
 * @brief 表行插入：列名仅允许 [A-Za-z0-9_]，值会做 mysql 转义。
 */
class MysqlMapper
{
public:
    explicit MysqlMapper(util::tagDbConnInfo conn);

    void insert(const std::string& table,
                const std::map<std::string, std::string>& row,
                MysqlInsertOk on_ok,
                MysqlErr on_err);

    std::future<std::uint64_t> insertFuture(const std::string& table,
                                            const std::map<std::string, std::string>& row);

private:
    util::tagDbConnInfo conn_;
};

} // namespace thunder::orm

#endif
