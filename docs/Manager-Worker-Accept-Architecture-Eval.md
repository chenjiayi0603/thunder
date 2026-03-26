# Manager/Worker Accept 架构评估报告

## 1. 当前实现基线

### 1.1 数据面分工
- `Manager` 持有监听 socket，并在 `IoRead()` 分支中处理 `accept`。
- C2S 新连接走 `FdTransfer()`：`accept` 后按最小负载 worker 选择目标，使用 `send_fd_with_attr` 透传 fd 给 worker。
- S2S 新连接走 `AcceptServerConn()`：`accept` 后继续由 manager 管理连接。
- `Worker` 不直接监听外部端口，只从 `iManagerDataFd` 读取透传的 fd（`recv_fd_with_attr`）并建立连接属性。

### 1.2 关键调用链（代码位置）
- 监听建立：`code/Net/src/labor/Manager.cpp` `Init()`
- `accept` 分发入口：`code/Net/src/labor/Manager.cpp` `IoRead()`
- C2S accept+分发：`code/Net/src/labor/Manager.cpp` `FdTransfer()`
- 最小负载选 worker：`code/Net/src/labor/Manager.cpp` `GetMinLoadWorkerDataFd()`
- worker 接收透传 fd：`code/Net/src/labor/Worker.cpp` `FdTransfer()`
- manager 接收 worker 上报负载：`code/Net/src/labor/Manager.cpp` `DisposeDataFromWorker()`

### 1.3 当前模型优势与风险
- 优势
  - 连接入口集中，连接准入策略（IP 频控、codec 注入）统一。
  - worker 重启/替换时，外部监听逻辑不需要直接变动。
- 风险
  - manager 成为 C2S accept 热点与潜在单点 CPU 瓶颈。
  - 每次连接有一次额外 FD 透传与 IPC 开销（manager<->worker data fd）。
  - C2S 与 S2S 模型不对称，复杂度上升（一部分流量在 manager，另一部分在 worker）。

## 2. 行业对标结论（Nginx/Envoy）

### 2.1 Nginx
- Nginx 多 worker 直接 accept 是成熟路线。
- 历史上使用 `accept_mutex` 轮转接受连接，降低惊群。
- 在 `EPOLLEXCLUSIVE` 或 `reuseport` 条件下，通常不再需要 `accept_mutex`。

### 2.2 Envoy
- 单进程多 worker 线程模型，worker 直接监听/accept。
- 默认依赖内核进行连接均衡，必要时启用显式连接均衡策略。
- 控制面线程不承载高频 accept 热路径。

### 2.3 对本项目的含义
- 如果目标是长期扩容并降低 manager 热点，方向应趋向“worker 自行 accept”。
- 如果当前瓶颈不在建连路径，保留 manager accept 也可接受，但应补监控与压测证据。

## 3. 统一压测设计（可对比 A/B/C）

## 3.1 场景与变量
- 场景S1：短连接洪峰（高建连率，低请求体积）。
- 场景S2：长连接稳态（连接建立后低频请求）。
- 场景S3：混合流量（70% 长连接 + 30% 短连接）。
- 固定变量：硬件、内核参数、worker 数、backlog、协议编解码设置。
- 对比变量：Accept 架构（A/B/C）。

### 3.2 指标口径
- 建连吞吐：`conn/s`（成功三次握手并完成业务首包握手）。
- 建连时延：P50/P99/P999。
- 均衡性：worker 间连接数标准差、Gini 系数。
- 资源：manager 与 worker 的 CPU/上下文切换/软中断占比。
- 稳定性：accept error、fd 透传失败率、连接重置率、重启恢复时间。

### 3.3 采样与判定
- 每场景至少 3 轮，每轮 >= 10 分钟；取中位数。
- 显著收益判定建议：
  - P99 建连时延下降 >= 15%
  - 或 manager CPU 峰值下降 >= 20%
  - 且错误率不升高（<= 基线+5% 相对值）

### 3.4 观测点建议
- manager：
  - `FdTransfer` 调用速率、失败数。
  - `send_fd_with_attr` 失败分类。
  - 负载选择结果分布（目标 worker 命中占比）。
- worker：
  - `recv_fd_with_attr` 成功/失败。
  - 接收后连接初始化耗时。
  - 业务处理线程/事件循环滞后指标。

## 4. 方案对比（A/B/C）

| 方案 | 描述 | 性能潜力 | 实施复杂度 | 风险 |
| --- | --- | --- | --- | --- |
| A | 保留 manager accept + fd 分发 | 中 | 低 | manager 热点持续存在 |
| B | 共享 listen fd，worker 直接 accept | 中-高 | 中 | 可能惊群/分配不均，需额外协调 |
| C | 每 worker 独立 listen + `SO_REUSEPORT` | 高 | 中-高 | 需重构监听生命周期与发布流程 |

结论：
- 短期稳妥：A（先补证据与指标）。
- 中长期推荐：C（与主流高并发入口模型一致，降低 manager 数据面负担）。
- B 作为过渡可行，但长期通常会继续演进到 C。

## 5. 推荐路线与实施阶段

### 阶段 0：证据化（立即）
- 在现有 A 模型加指标与日志，完成统一压测基线。
- 明确 manager 是否已成为 accept/建连瓶颈。

### 阶段 1：低风险优化 A
- 优化最小负载选择策略（考虑连接数与实时负载混合权重）。
- 对 fd 透传失败分类重试与熔断，提升可观测性。

### 阶段 2：灰度 C（建议目标）
- 新增 `accept_mode` 配置：`manager_dispatch` / `worker_reuseport`。
- 小流量灰度 `worker_reuseport`，对比同场景指标。
- 达到判定阈值后逐步扩大流量。

### 阶段 3：收敛
- 若 C 稳定且收益明确，逐步下线 manager C2S accept 热路径。
- manager 保留控制面职责：进程管理、配置、路由/中心协调。

## 6. 回滚策略

- 运行时保留双模式开关：
  - 首选配置回滚到 `manager_dispatch`。
  - 保留旧链路代码直到 C 连续多个版本稳定。
- 触发回滚条件（任一满足）：
  - P99 建连时延恶化 > 20%
  - 连接错误率升高 > 10%（相对基线）
  - worker 间连接极不均衡且持续超过设定阈值
- 回滚步骤
  - 切换模式 -> reload 配置 -> 观察 2 个统计周期 -> 必要时重启 worker 清理状态。

## 7. 最终建议

- 你们当前代码下，不应直接“拍脑袋”把 accept 全搬到 worker。
- 正确顺序是：先证据化（阶段0），再灰度化演进（阶段2）。
- 若目标是长期高并发扩展，建议明确以 C（`SO_REUSEPORT`）为目标架构；A 作为当前稳定基线与回滚兜底。

## 8. FAQ：三种 fd 分配语义

### 8.1 当前 A：manager 分配给 worker 的策略
- 策略类型：用户态调度。
- 实现方式：`Manager::FdTransfer()` 中 `accept` 成功后，调用 `GetMinLoadWorkerDataFd()` 选择 `iLoad` 最小的 worker，再通过 `send_fd_with_attr` 透传 fd。
- 负载来源：worker 周期上报 `CMD_REQ_UPDATE_WORKER_LOAD`，manager 维护 `m_mapWorker[*].iLoad`；新连接派发成功后会先 `AddWorkerLoad(+1)` 做即时修正。
- `iLoad` 含义：不是纯连接数。当前口径是 `mapFdAttr.size() + mapCallbackStep.size()`，即“连接相关 fd 数 + 在途回调/步骤数”的组合负载估计。
- 含义：连接归属由 manager 决定，内核不参与“选哪个 worker”。

### 8.2 B：共享 listen fd 的含义
- 含义：多个 worker 进程监听同一个 listen fd（通常由 manager 先创建并在 fork 后继承）。
- 行为：每个 worker 都对同一个监听 fd 等待可读并调用 `accept`；谁抢到连接由内核调度决定。
- 特点：无 manager fd 透传开销，但可能出现惊群/分配不均（取决于内核机制与配置，比如 `EPOLLEXCLUSIVE`）。

### 8.3 C：每 worker 独立 listen + `SO_REUSEPORT` 的内核策略
- 含义：每个 worker 自己创建 listen socket，并开启 `SO_REUSEPORT` 后绑定到同一 IP:port，形成 reuseport 组。
- 行为：新连接到来时，内核先在 reuseport 组中挑选一个目标 listen socket，再把连接放入该 socket 的 accept 队列，对应 worker 处理。
- 选择逻辑：由内核哈希/负载分流机制决定（与连接元组等因素有关），目标是降低锁竞争并获得更好的并行 accept 伸缩性。
- 关键差异：该模型不是 manager 那种“按业务 `iLoad` 最小值”分配，而是“内核哈希近似均衡”。
- 一致性特征：同一客户端在连接元组稳定时，通常更容易落到同一监听 socket；但并非强保证，源端口/NAT/进程重启等变化都可能改变落点。

### 8.4 为什么 C 通常优于 A
- 热路径更短：A 是 `accept -> send_fd -> worker`，C 是 `accept -> worker`，减少一次跨进程 fd 透传。
- 消除 manager 建连热点：A 的新连接都经过 manager；C 由 worker 并行承接，manager 可专注控制面。
- 扩展性更好：C 下内核直接对多个监听 socket 做分流，通常比用户态串行入口再分发更容易扩容。
- 故障域更小：A 中 manager 抖动直接影响全局建连；C 中单 worker 异常对全局入口影响更可控。
- 适配主流实践：高并发系统普遍将监听/accept 放在数据面 worker，而不是控制面进程。
- 边界说明：C 的分配是“内核近似均衡”，不是“按业务 `iLoad` 精准派发”；若业务强依赖精确负载路由，需要在上层补充策略。

### 8.5 一句话结论
- 一般情况下，C（`SO_REUSEPORT`）的并发 accept 能力更强，也更符合业界主流方向。
- 但工程决策仍应以本项目压测结果为准：若当前瓶颈不在建连路径，A 可能在阶段性更具性价比。

## 9. C方案落地说明（当前代码）

### 9.1 配置要求
- 当前代码已收敛为仅 C 方案，不再保留 manager 接受客户端连接路径。
- 只需提供客户端监听相关配置：`access_host`、`access_port`、`client_socket_backlog`、`access_codec`。

### 9.2 当前实现行为
- `Manager` 永远跳过 C2S 监听，仅保留控制面与 S2S。
- `Worker` 固定执行 C 方案：
  - `socket + SO_REUSEPORT + bind + listen`
  - 监听 fd 可读后直接 `accept`
  - 按 `access_codec` 初始化连接编解码

### 9.3 注意事项
- 本阶段只迁移 C2S 入口，S2S 保持原设计。
- 若内核/构建环境不支持 `SO_REUSEPORT`，worker 会启动失败并记录错误日志。
