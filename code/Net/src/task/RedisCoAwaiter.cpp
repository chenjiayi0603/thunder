#include "task/RedisCoAwaiter.hpp"
#include "task/BaseTask.hpp"

namespace net {

void RedisCommandAwaiter::await_suspend(std::coroutine_handle<> handle) {
    task_->SaveCoroutineHandle(handle);
    task_->ResumeWithError(-1, "RedisCommandAwaiter: integrate with RedisStep/async pipeline in follow-up work");
}

RedisResponse RedisCommandAwaiter::await_resume() const {
    RedisResponse r;
    r.type = RedisResponse::Type::ERROR;
    r.errNo = task_->GetLastErrno();
    r.errMsg = task_->GetLastErrMsg();
    (void)host_;
    (void)port_;
    (void)command_;
    return r;
}

RedisCommandAwaiter RedisCoHelper::Get(const std::string& key) {
    return RedisCommandAwaiter(task_, host_, port_, "GET " + key);
}

RedisCommandAwaiter RedisCoHelper::Set(const std::string& key, const std::string& value) {
    return RedisCommandAwaiter(task_, host_, port_, "SET " + key + " " + value);
}

RedisCommandAwaiter RedisCoHelper::Del(const std::string& key) {
    return RedisCommandAwaiter(task_, host_, port_, "DEL " + key);
}

RedisCommandAwaiter RedisCoHelper::HGet(const std::string& key, const std::string& field) {
    return RedisCommandAwaiter(task_, host_, port_, "HGET " + key + " " + field);
}

RedisCommandAwaiter RedisCoHelper::HSet(const std::string& key, const std::string& field,
                                        const std::string& value) {
    return RedisCommandAwaiter(task_, host_, port_, "HSET " + key + " " + field + " " + value);
}

RedisCommandAwaiter RedisCoHelper::Expire(const std::string& key, int seconds) {
    return RedisCommandAwaiter(task_, host_, port_, "EXPIRE " + key + " " + std::to_string(seconds));
}

RedisCommandAwaiter RedisCoHelper::Execute(const std::string& command) {
    return RedisCommandAwaiter(task_, host_, port_, command);
}

} // namespace net
