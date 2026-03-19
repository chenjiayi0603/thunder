# Thunder 框架：StepState 回调模式改为 C++20 协程

## Context

Thunder（D:\interview-quicker1\thunder） 是一个基于 libev 的全异步 C++ 网络框架，当前使用 **StepState 状态机模式** 处理异步操作：每个状态函数执行一个异步操作，返回 `STATUS_CMD_RUNNING`，等待框架回调后进入下一个状态。这种模式导致：
- 业务逻辑分散在多个 State 函数中（State0/State1/State2...）
- 状态跳转靠 `SetNextState()`/`JumpNextState()`，流程难以一目了然
- 需要手动注册状态函数数组

目标：用 C++20 协程（`co_await`/`co_return`）替换 StepState 模式，使异步代码写起来像同步代码。
D:\interview-quicker1\thunder\code\Net\src\step 会去掉，后面可以考虑增加协程对象类型保存上下文

可以参考 D:\interview-quicker1\drogon 里面的协程使用

先分析，制定计划，然后再执行