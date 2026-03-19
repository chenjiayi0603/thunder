#include "StepHttpRequestCo.hpp"
#include "HttpMsg.hpp"

namespace hello {

StepHttpRequestCo::StepHttpRequestCo() {}

StepHttpRequestCo::~StepHttpRequestCo() {}

thunder::CoTask StepHttpRequestCo::Run() {
    // 第一个 HTTP 请求
    auto resp1 = co_await HttpGetAsync("http://baidu.com");
    // 处理 resp1...

    // 第二个 HTTP 请求
    auto resp2 = co_await HttpGetAsync("http://sogou.com");
    // 处理 resp2...

    // 响应客户端
    Response(0);

    co_return;
}

}  // namespace hello
