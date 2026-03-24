# StepCo20 协程 + 线程池：例 A / 例 B（伪代码）

本文件与主设计文档 [docs/StepCo20-threadpool-integration-design.md](../../../../docs/StepCo20-threadpool-integration-design.md) 第 4.2 节对应，放在 Hello 的 ModuleHello 旁便于和协程示例插件对照阅读。

说明：PoolOffloadAwaiter、g_threadpool 全局实例、ScheduleBackToWorker 等在本仓库均为设计/伪代码层面；commit 的真实实现见 Util/src/thread/threadpool.h（队列 + mutex + condition_variable）。

---

## 例 A：重 CPU（与主文档 §4.1 同一 awaiter 形态）

```cpp
// 假设 HeavyCompress 是同步、纯 CPU、可能很慢
AsyncTask BodyCpu(StepCo20& step) {
    std::vector<uint8_t> raw = CopyRequestBytes(step);
    auto out = std::make_shared<CpuJobResult>();

    co_await PoolOffloadAwaiter(&step, g_threadpool, std::move(raw), out,
        [](std::vector<uint8_t> buf, std::shared_ptr<CpuJobResult> o) {
            o->bytes = HeavyCompress(buf); // 仅池线程执行
            o->err = 0;
        });

    step.NotifyEmitCoroutineSuccess();
    co_return;
}

// 反例：不要这样，事件线程会被占死
AsyncTask BodyCpuBad(StepCo20& step) {
    std::vector<uint8_t> raw = CopyRequestBytes(step);
    auto packed = HeavyCompress(raw); // 错：在事件线程跑重 CPU
    step.ResponseToClient(200, packed);
    step.NotifyEmitCoroutineSuccess();
    co_return;
}
```

---

## 例 B：同步外调（阻塞 SDK，无异步接口）

```cpp
// 假设 ThirdPartyBlockingFetch 会阻塞等网络，无异步接口
AsyncTask BodySyncApi(StepCo20& step) {
    std::string url = CopyUrl(step);
    std::string postBody = CopyPostBody(step);
    auto out = std::make_shared<SyncApiResult>();

    co_await PoolOffloadAwaiter(&step, g_threadpool, url, postBody, out,
        [](std::string u, std::string body, std::shared_ptr<SyncApiResult> o) {
            o->response = ThirdPartyBlockingFetch(u, body); // 仅池线程阻塞
            o->err = 0;
        });

    step.NotifyEmitCoroutineSuccess();
    co_return;
}

// PoolOffloadAwaiter 内部：池线程里 work(url, postBody, out)；
// 末尾 ScheduleBackToWorker 里读 out、ResponseToClient、resume(h)。详见主文档 §4.1。
```

---

## 与 ModuleHello 里已有协程示例的关系

ModuleHello 中 TestStepHttpRequestCo、TestHttpRequestCo 等演示的是 StepCo20 + HttpGetAsync 等**已有异步路径**，未接线程池。若将来在 Hello 里做「重 CPU / 阻塞 SDK」演示，可按上列伪代码接入 threadpool::commit + 投递回 Worker，并先实现 PoolOffloadAwaiter 或等价封装。
