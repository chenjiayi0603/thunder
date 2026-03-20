#include "StepHttpRequestCo.hpp"

namespace hello
{

StepHttpRequestCo::StepHttpRequestCo() = default;

StepHttpRequestCo::~StepHttpRequestCo() = default;

net::CoTask StepHttpRequestCo::Run()
{
    (void)co_await HttpGetAsync("http://www.baidu.com/");
    (void)co_await HttpGetAsync("http://www.sogou.com/");
    co_return;
}

} // namespace hello
