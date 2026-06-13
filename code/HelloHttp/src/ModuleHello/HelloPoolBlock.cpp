#include "HelloPoolBlock.hpp"
#include <chrono>
#include <thread>
#include "coro/StepCo20.hpp"
#include "coro/ThreadPoolAwaitable.hpp"
#include "util/CommonUtils.hpp"

net::AsyncTask HelloPoolBlockCo(net::StepCo20& step)
{
	const int delay_ms = 80;
	const int delay_ms2 = delay_ms + 1;
	const int result = co_await net::MakePoolOffloadAwaiter(
		&step,
		[](int d1, int d2) -> int {
			// 典型场景：在线程池子线程中调用无状态、会阻塞的外部 IO 同步 SDK 或函数（此处 sleep 仅作演示）。
			// 同上约束：不得访问 Step/事件线程资源或未经同步的共享数据。
			(void)d2;
			std::this_thread::sleep_for(std::chrono::milliseconds(d1));
			return d1 + d2;
		},
		delay_ms, delay_ms2);
	util::CJsonObject j;
	j.Add("option", "TestHelloPoolBlock");
	j.Add("slept_ms", delay_ms);
	j.Add("result", result);
	step.ResponseToClient(200, j.ToString());
	co_return;
}
