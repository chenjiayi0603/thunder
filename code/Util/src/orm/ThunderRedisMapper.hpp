/*******************************************************************************
 * Thunder 轻量 Redis KV 封装（参考 Drogon ORM 的异步回调 + future 形态）
 * @note 后台线程阻塞 hiredis；业务 EventLoop 内请继续用 RedisStep。
 ******************************************************************************/
#ifndef THUNDER_ORM_REDIS_MAPPER_HPP_
#define THUNDER_ORM_REDIS_MAPPER_HPP_

#include <functional>
#include <future>
#include <string>

namespace thunder::orm
{

using RedisOk = std::function<void()>;
using RedisErr = std::function<void(const std::string& what)>;

class RedisMapper
{
public:
    RedisMapper(std::string host, int port);

    void set(const std::string& key, const std::string& value, RedisOk on_ok, RedisErr on_err);

    std::future<void> setFuture(const std::string& key, const std::string& value);

    std::future<std::string> getFuture(const std::string& key);

private:
    std::string host_;
    int port_{6379};
};

} // namespace thunder::orm

#endif
