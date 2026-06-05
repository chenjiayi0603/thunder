# Center 对比 Nacos 的取舍评估

> 日期: 2026-06-02
> 状态: 评估 / 设计论证
> 驱动问题: 既然有 Nacos 这类成熟注册配置中心，Thunder 自研 Center 到底有没有价值？
> 结论先行: **论"通用注册配置中心"的功能丰富度，Center 完败 Nacos；但 Center 换来的是「零 JVM 依赖」+「配置 shm 零跳直推 Worker」两项极致集成优势——这是用通用性换集成度，只在 Thunder 这种自洽高性能 C++ 框架里才划算。真正该质疑的不是「Center vs Nacos」，而是「自研 Raft 选主是否值得」，因为上述两项优势都不依赖自研 Raft。**

---

## 1. 背景与对比对象的选择

Thunder 是主打低延迟的 C++20 框架，在 IoBackend（io_uring/epoll）、协程帧开销、shm 零拷贝上层层抠性能。注册/配置中心若引入一套与框架调性不符的重组件，会从部署形态和延迟基线上拉低整体水平。

Nacos 是 **JVM 系**通用注册配置中心，功能齐全但运行时重。严格说，与 Center（C++ 单二进制、同业务节点形态）同量级的对手是 etcd / Consul（Go 单二进制）。但本评估按用户提出的命题，直接对比 **Center vs Nacos**，并指出 JVM 这一关键差异。

---

## 2. 功能维度：Center 全面落后 Nacos

| 能力 | Nacos | Thunder Center |
|------|-------|----------------|
| 服务注册 / 发现 | ✅ 成熟 | ✅ 有，仅够自用 |
| 配置管理 | ✅ 控制台编辑、版本 / 灰度 / 回滚 | ⚠️ 仅 shm 下发，无管理面 |
| 健康检查 | ✅ 心跳 + 主动探测 + 权重 | ⚠️ 断连检测（35s 心跳超时摘除），较粗 |
| 多语言 SDK | ✅ Java / Go / C++ / Python… | ❌ 仅服务 Thunder 自身 |
| Web 控制台 | ✅ 完整 | ⚠️ Admin 页，简陋 |
| AP / CP 切换 | ✅ Distro(AP) / Raft(CP) | ❌ 仅 Raft 一条路 |
| 生产工具链 / 监控 | ✅ 生态齐全 | ❌ 几乎没有 |

**结论：以"通用注册配置中心"论，Center 没有任何功能优势，需要开箱即用就直接上 Nacos。** Center 的价值不在这张表里——它不是来抢通用件的位置的。

---

## 3. Center 的两项不可替代优势

### 3.1 配置 shm 零跳直推 Worker

**机制**（CLAUDE.md 关键架构决策）：

```
主 Center → 业务节点 Loader → 共享内存(shm) → Worker 定时检查 version → 增量更新
```

Manager 写配置 blob → `version++`（原子）→ Worker 定时读 version，变化则本地重读 blob。

对比"Worker 从 Nacos 网络拉配置"，具体好处：

1. **延迟差 3~4 个数量级**
   - shm 读 = 本地内存 `memcpy` + 原子 load version，**纳秒级**。
   - Nacos 拉取 = 一次 TCP RPC + 反序列化（+ 长轮询），同机房也是**百微秒~毫秒级**。
   - 热路径上读配置走网络，与框架抠 io_uring/协程开销的取向矛盾。

2. **一份配置，N 个 Worker 零拷贝共享**
   - 一节点上 N 个 Worker 进程 attach **同一块** shm，配置只存一份，Manager 写一次全员可见。
   - Nacos 模型下每个 Worker 各自连接、各自拉取、各自反序列化、各自存一份 → N 倍网络 + N 份内存。

3. **fork 多进程模型原生契合**
   - Thunder 是 Manager/Loader/Worker 多进程，Worker fork 出来直接 attach shm。
   - Nacos SDK 是"一进程一客户端 + 后台长轮询线程"设计。fork 后子进程不继承父线程，长轮询线程丢失，需在每个 Worker 重建客户端、重连——与 fork 模型冲突。

4. **Center 宕机 Worker 不死**
   - 配置在本地 shm，Center 宕机 / 网络抖动时 Worker 读本地内存照常运行（"超限不写入、打错误日志、Worker 继续用旧版本"）。
   - Nacos 也有本地快照兜底，但那是**落盘文件**，读盘 vs 读内存又差一个数量级，且需处理快照过期。

5. **半包一致性自己掌控**
   - 布局：blob → len → version++（原子递增）。先写 blob 再写 len，Worker 永不读到半个配置。节点内一致性完全可控。

**代价（具体）**：shm 固定 160KB（路由 160KB / 配置 160KB），超限不下发；只解决**节点内 Center→Worker** 这一跳，跨机仍需 Center 走网络发到各节点 Loader；轮询有感知周期，非即时推送。

### 3.2 无 JVM

要用 Nacos，需**额外部署一套 Java 服务**（生产 3 节点 = 3 个 JVM 实例）。具体代价：

1. **内存：GB 级 vs 几十 MB**
   - Nacos 单节点官方建议 2C4G，JVM 堆 1~2GB 起；3 节点 = 3~6GB 常驻，纯给注册中心。
   - Center 是 C++ 二进制，几十 MB 量级，与业务节点同形态。

2. **无 GC 停顿，P99 可预测**
   - JVM 有 STW；注册风暴 / 配置变更风暴时 GC 抖动会拉高注册和推送尾延迟。
   - C++ 无 GC，延迟基线稳，与框架"主打 P99"调性一致。

3. **技术栈不分裂**
   - 全栈 C++ 单二进制，`nodes.sh` 起停，一个 `deploy/` 目录。
   - 引入 Nacos = 团队需装 JDK、调 `-Xmx/-Xms`/GC 策略、读 GC 日志、上 JMX 监控，运维复杂度翻倍。

4. **Docker / CI 更轻**
   - C++ 镜像几十 MB、毫秒级启动；Nacos 镜像几百 MB 带 JRE、有 JIT 预热冷启动慢。
   - Thunder E2E 是 Docker 8 服务，混入 Nacos 后起容器时间和资源都上涨。

5. **攻击面更小**
   - Nacos 历史踩过反序列化 RCE（Fastjson/Hessian 类）、默认 `nacos/nacos` 未授权访问。JVM 反序列化是长期漏洞面。
   - Center 是自有 C++ 二进制协议，攻击面小且自己掌控。

---

## 4. 诚实的边界：优势何时不成立

- 若业务节点本就是 **Java 栈**、运维已养着 JVM，"无 JVM"是沉没收益，省不到。
- 若节点少、配置基本不变，shm 的纳秒级优势**感知不到**，Nacos 完全够用。
- shm 直推真正发光场景：**单机多 Worker + 高频读配置 + 极致 P99 + 不引第二种运行时**——即 Thunder 自身。

---

## 5. 结论与后续

| 维度 | 判断 |
|------|------|
| Center vs Nacos（功能） | Center 完败，需通用件就上 Nacos |
| Center 的不可替代项 | ① 无 JVM（部署形态统一 + 延迟基线稳 + 不引第二运行时）② shm 零跳直推（节点内读配置纳秒级 + 多进程零拷贝共享） |
| 自研 Raft 选主是否值得 | **存疑**——上述两项优势均不依赖自研 Raft；纯工程性价比为负，学习 / 作品集维度为正 |

**收口**：shm 直推买的是"节点内读配置零跳延迟 + 多进程零拷贝共享"；无 JVM 买的是"部署形态统一 + 延迟基线稳 + 不引入第二套运行时"。两者都是"用通用性换极致集成"，只在 Thunder 这种自洽高性能框架里才划算。

下一步的真正命题是**「能否在保留这两项优势的前提下去掉自研强一致中心」**——即 [gossip_decentralization_evaluation.md](./gossip_decentralization_evaluation.md) 与 [raft_leader_lease_design.md](./raft_leader_lease_design.md) 在探索的方向。
