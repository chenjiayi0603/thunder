/*******************************************************************************
 * @file ThreadPoolAwaitable.hpp
 * @brief StepCo20 co_await：重 CPU / 阻塞调用卸载到 util::threadpool，经 Labor::PostToEventLoop 回 Worker
 * @note 与 future_awaiter_demo 的 FutureAwaiter 类似：awaiter 内持有 std::future；任务在线程池内执行。
 *       本处 future 在池任务 return 时即就绪（任务内已 PostToEventLoop），故事件线程上 await_resume 里
 *       fut_.get() 不会阻塞。勿在事件线程上对未就绪的 future 调用 get()。
 *       PoolOffloadAwaiter 持有 std::future<ResultT>；RunOnThreadPool 为其无参 body/out（双 monostate）封装。
 ******************************************************************************/
#ifndef NET_CORO_THREAD_POOL_AWAITABLE_HPP_
#define NET_CORO_THREAD_POOL_AWAITABLE_HPP_

#include "coro/StepCo20.hpp"
#include "labor/Labor.hpp"
#include "labor/Worker.hpp"
#include "labor/WorkerThreadPool.hpp"
#include "log/CustomLogger.hpp"
#include "thread/threadpool.h"

#include <coroutine>
#include <future>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace net
{

/**
 * @brief 在线程池执行 work(std::move(body), out)，返回值经 packaged_task 写入 std::future<ResultT>；
 *        await_resume 中 get() 取得 ResultT 并传播异常（ResultT 可为 void）
 */
template <class BodyT, class OutT, class WorkFn>
struct PoolOffloadAwaiter
{
	using ResultT = std::invoke_result_t<WorkFn, BodyT&&, std::shared_ptr<OutT>&>;

	StepCo20* step_ = nullptr;
	util::threadpool* pool_ = nullptr;
	BodyT body_;
	std::shared_ptr<OutT> out_;
	WorkFn work_;
	uint32 stepSeq_ = 0;
	std::future<ResultT> fut_{};

	PoolOffloadAwaiter(StepCo20* s, util::threadpool& p, BodyT&& b, std::shared_ptr<OutT> o, WorkFn w)
		: step_(s), pool_(&p), body_(std::move(b)), out_(std::move(o)), work_(std::move(w))
	{
		if (step_ != nullptr)
		{
			stepSeq_ = step_->GetSequence();
		}
	}

	bool await_ready() const noexcept { return false; }

	void await_suspend(std::coroutine_handle<> h)
	{
		if (step_ == nullptr || pool_ == nullptr)
		{
			// 无可用线程池，不能挂起（无人 resume），直接恢复让 await_resume 处理
			if (h && !h.done())
			{
				h.resume();
			}
			return;
		}
		step_->m_context.handle = h;
		BodyT body = std::move(body_);
		std::shared_ptr<OutT> out = out_;
		WorkFn work = std::move(work_);
		StepCo20* step = step_;
		const uint32 seq = stepSeq_;
		util::threadpool* pool = pool_;
		try
		{
			fut_ = pool->commit([body = std::move(body), out, work = std::move(work), step, seq, h]() mutable
								  -> ResultT {
				auto resumeOnWorker = [step, seq, h]() {
					Worker* wk = dynamic_cast<Worker*>(GetLabor());
					if (wk == nullptr || step == nullptr)
					{
						return;
					}
					if (!wk->IsRegisteredStep(seq, step))
					{
						LOG4_TRACE("PoolOffloadAwaiter: step seq=%u unregistered, skip resume", seq);
						return;
					}
					if (h && !h.done())
					{
						h.resume();
					}
				};
				if constexpr (std::is_void_v<ResultT>)
				{
					work(std::move(body), out);
					if (Labor* labor = GetLabor())
					{
						labor->PostToEventLoop(resumeOnWorker);
					}
					return;
				}
				else
				{
					ResultT r = work(std::move(body), out);
					if (Labor* labor = GetLabor())
					{
						labor->PostToEventLoop(resumeOnWorker);
					}
					return r;
				}
			});
		}
		catch (...)
		{
			LOG4_ERROR("PoolOffloadAwaiter: threadpool commit failed");
			// 任务未入队，fut_ 保持 invalid。直接 resume 协程，
			// await_resume 会检查 fut_.valid() 并抛异常通知调用方。
			if (h && !h.done())
			{
				h.resume();
			}
		}
	}

	decltype(auto) await_resume()
	{
		if constexpr (std::is_void_v<ResultT>)
		{
			if (fut_.valid())
			{
				fut_.get();
			}
		}
		else
		{
			if (!fut_.valid())
			{
				throw std::runtime_error("PoolOffloadAwaiter: invalid future");
			}
			return fut_.get();
		}
	}
};

template <class BodyT, class OutT, class WorkFn>
PoolOffloadAwaiter<BodyT, OutT, std::decay_t<WorkFn>> MakePoolOffloadAwaiter(
	StepCo20* s, util::threadpool& p, BodyT&& b, std::shared_ptr<OutT> o, WorkFn&& w)
{
	return PoolOffloadAwaiter<BodyT, OutT, std::decay_t<WorkFn>>(
		s, p, std::forward<BodyT>(b), std::move(o), std::forward<WorkFn>(w));
}

/** @brief 使用 Worker 全局线程池（ThunderWorkerThreadPool） */
template <class BodyT, class OutT, class WorkFn>
PoolOffloadAwaiter<BodyT, OutT, std::decay_t<WorkFn>> MakePoolOffloadAwaiter(
	StepCo20* s, BodyT&& b, std::shared_ptr<OutT> o, WorkFn&& w)
{
	return MakePoolOffloadAwaiter(
		s, ThunderWorkerThreadPool(), std::forward<BodyT>(b), std::move(o), std::forward<WorkFn>(w));
}

/**
 * @brief 无 out：work 紧跟 pool（避免 BodyTs..., WorkFn&& 推导把包推成空）；其后 0～多个 body；签名为 R(BodyTs...) / void(BodyTs...)
 */
template <class WorkFn, class... BodyTs,
	std::enable_if_t<std::is_invocable_v<std::decay_t<WorkFn>, BodyTs...>, int> = 0>
auto MakePoolOffloadAwaiter(StepCo20* s, util::threadpool& p, WorkFn&& w, BodyTs&&... bodyArgs)
{
	using BodyPack = std::tuple<std::decay_t<BodyTs>...>;
	// std::decay_t 是 C++ 标准库类型萃取工具，通常用于将类型参数变为它的“原始”形式（去除引用、const、volatile、数组等），常用于泛型编程以获得更适合实例化/存储的类型。
	using W = typename std::decay<WorkFn>::type;
	// std::invoke_result_t 是 C++17 提供的类型萃取，用于获得“用类型为 W 的可调用对象，以 BodyTs... 作为参数调用后返回值类型”
	using R = std::invoke_result_t<W, BodyTs...>;
	auto body = std::make_tuple(std::forward<BodyTs>(bodyArgs)...);
	auto workAdapter = [w = std::forward<WorkFn>(w)](BodyPack body, std::shared_ptr<std::monostate>) mutable -> R {
		if constexpr (std::is_void_v<R>)
		{
			std::apply(
				[&w](auto&&... parts) { w(std::forward<decltype(parts)>(parts)...); }, std::move(body));
		}
		else
		{
			return std::apply(
				[&w](auto&&... parts) -> R { return w(std::forward<decltype(parts)>(parts)...); },
				std::move(body));
		}
	};
	return PoolOffloadAwaiter<BodyPack, std::monostate, decltype(workAdapter)>(
		s, p, std::move(body), std::make_shared<std::monostate>(), std::move(workAdapter));
}

/** @brief 无 out：使用 Worker 全局线程池（ThunderWorkerThreadPool） */
template <class WorkFn, class... BodyTs,
	std::enable_if_t<std::is_invocable_v<std::decay_t<WorkFn>, BodyTs...>, int> = 0>
auto MakePoolOffloadAwaiter(StepCo20* s, WorkFn&& w, BodyTs&&... bodyArgs)
{
	return MakePoolOffloadAwaiter(
		s, ThunderWorkerThreadPool(), std::forward<WorkFn>(w), std::forward<BodyTs>(bodyArgs)...);
}

/** @brief 池内执行无参 lambda 并返回值（非 void）；等价于 MakePoolOffloadAwaiter(&step, pool, f) */
template <class F>
auto RunOnThreadPool(StepCo20& step, util::threadpool& pool, F&& f)
{
	using R = std::invoke_result_t<std::decay_t<F>>;
	static_assert(!std::is_void_v<R>, "RunOnThreadPool: return type must not be void; use MakePoolOffloadAwaiter (work may return void)");
	return MakePoolOffloadAwaiter(&step, pool, std::forward<F>(f));
}

} // namespace net

#endif
