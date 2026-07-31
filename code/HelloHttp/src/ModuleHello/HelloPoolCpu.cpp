#include "HelloPoolCpu.hpp"
#include <cstdint>
#include <vector>
#include "coro/StepCo20.hpp"
#include "coro/ThreadPoolAwaitable.hpp"
#include "util/CommonUtils.hpp"

net::AsyncTask HelloPoolCpuCo(net::StepCo20& step)
{
	std::vector<uint8_t> buf(256 * 1024, static_cast<uint8_t>(3));
	const uint64_t checksum = co_await net::MakeThreadPoolAwaiter(
		&step,
		[](std::vector<uint8_t> b) -> uint64_t {
			// 运行于线程池子线程（非 Worker/libev 线程）：勿用 GetLabor、ResponseToClient、
			// 未同步的共享可变状态及非线程安全接口；仅处理入参副本，结果通过返回值传出。
			uint64_t s = 0;
			for (uint8_t x : b)
			{
				s += x;
			}
			return s;
		},
		std::move(buf));
	util::CJsonObject j;
	j.Add("option", "TestHelloPoolCpu");
	j.Add("checksum", static_cast<util::int64>(checksum));
	step.ResponseToClient(200, j.ToString());
	co_return;
}
