#include "task/HttpAwaiter.hpp"
#include "task/HttpTask.hpp"
#include <stdexcept>

namespace net {

void HttpGetAwaiter::await_suspend(std::coroutine_handle<> handle) {
    task_->ClearPendingError();
    task_->SaveCoroutineHandle(handle);
    if (!task_->HttpGet(url_)) {
        task_->ResumeWithError(-1, "HttpGet send failed");
    }
}

HttpResponse HttpGetAwaiter::await_resume() const {
    if (task_->GetCancellationToken().IsCancelled())
        throw std::runtime_error("task cancelled");
    if (task_->GetLastErrno() != 0)
        throw std::runtime_error(task_->GetLastErrMsg());
    return HttpResponse::FromHttpMsg(task_->GetLastHttpMsg());
}

HttpPostAwaiter::HttpPostAwaiter(HttpTask* task, std::string url, std::string body,
                                 std::unordered_map<std::string, std::string> headers)
    : task_(task), url_(std::move(url)), body_(std::move(body)), headers_(std::move(headers)) {}

void HttpPostAwaiter::await_suspend(std::coroutine_handle<> handle) {
    task_->ClearPendingError();
    task_->SaveCoroutineHandle(handle);
    if (headers_.empty()) {
        if (!task_->HttpPost(url_, body_)) {
            task_->ResumeWithError(-1, "HttpPost send failed");
        }
    } else {
        if (!task_->HttpPost(url_, body_, headers_)) {
            task_->ResumeWithError(-1, "HttpPost send failed");
        }
    }
}

HttpResponse HttpPostAwaiter::await_resume() const {
    if (task_->GetCancellationToken().IsCancelled())
        throw std::runtime_error("task cancelled");
    if (task_->GetLastErrno() != 0)
        throw std::runtime_error(task_->GetLastErrMsg());
    return HttpResponse::FromHttpMsg(task_->GetLastHttpMsg());
}

} // namespace net
