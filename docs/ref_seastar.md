
## 13. Seastar 是什么：定位、用途与 io_uring 关系

本节单独说明 **Seastar** 框架，便于与本文主题的 **Boost.Asio / io_uring**、**Thunder**、**nginx** 区分；内容可与 §8「业界参考」、§11 外部链接对照阅读。

### 13.1 Seastar 是什么

**Seastar** 是一套用 **C++** 编写的 **高并发服务器框架**，最初与 **ScyllaDB** 关系密切，上游源码托管于 **[scylladb/seastar](https://github.com/scylladb/seastar)**。

它面向的典型目标是：

- **单机多核** 上把 CPU 吃满且 **少锁**；
- **高吞吐**、**延迟相对可控**（适合对尾延迟敏感的数据面系统）。

常见落点包括：**数据库、消息、缓存** 等「内核型」服务进程，而不是轻量脚本级 Web 小工具。

### 13.1.1 ScyllaDB 与 Seastar 的关系

**[ScyllaDB](https://github.com/scylladb/scylladb)**（仓库 `scylladb/scylladb`）是与 **Apache Cassandra**、**Amazon DynamoDB** API 兼容的 **分布式 NoSQL 数据库**（shared-nothing、面向高吞吐与低延迟）。它与 Seastar 的关系可以概括为：

| 维度 | 说明 |
|------|------|
| **架构** | ScyllaDB **服务端二进制**在实现上 **建立在 Seastar 之上**：Seastar 提供 **每核 Reactor、网络、定时器、异步磁盘 I/O、跨 shard 通信、指标与部分基础设施**；ScyllaDB 在其上实现 **CQL、存储引擎、SSTable、复制与修复、compaction、集群成员** 等数据库层逻辑。 |
| **源码与构建** | **[ScyllaDB 仓库](https://github.com/scylladb/scylladb)** 以 **git submodule** 引入 Seastar（目录一般为 `seastar/`，指向上游 **[scylladb/seastar](https://github.com/scylladb/seastar)** 的固定提交），与主工程 **一起编译、链接进同一个 `scylla` 进程**，不是运行时可选的「外挂网络库」。 |
| **为何常被一起讲** | Seastar 由 Scylla 系项目推动演进；**ScyllaDB 是 Seastar 最大、最完整的公开产品级用例**，读 Seastar 文档时若需要对照真实代码路径，往往会落到 ScyllaDB 仓库。 |

**区分口径**：**Seastar** = 通用 C++ 多核异步框架（也可被其他项目使用）；**ScyllaDB** = 基于该框架实现的具体数据库产品。二者不是并列替代关系，而是 **框架 / 上层应用** 的关系。

### 13.1.2 ScyllaDB 与常见系统的边界（功能与协议）

读 Seastar 时往往会连带看到 **ScyllaDB**；若与团队里更常见的 **MySQL**、**Redis** 混谈，需要先划清 **数据模型** 与 **客户端协议** 的边界，避免「换库不换代码」的误判。

**与 MySQL、Redis 的对比（概要）**

| 维度 | MySQL（典型 InnoDB） | Redis | ScyllaDB |
|------|----------------------|-------|----------|
| **常见定位** | 关系型 OLTP：事务、复杂查询、强一致主路径 | 内存数据结构：缓存、会话、队列/流、计数 | 宽表 NoSQL：分区键驱动、高写入、水平扩展 |
| **数据模型** | 表 / 行 / 列；JOIN、约束、二级索引成熟 | 键 + String/Hash/List/Set/ZSet/Stream 等 | 宽表 + **partition key**；CQL 类似 SQL 子集，**不是**完整 SQL |
| **默认持久化语义** | 磁盘为主、事务日志 | 内存为主，RDB/AOF 为辅 | 磁盘-oriented，多副本 + 可调一致性级别 |
| **客户端协议** | **MySQL 协议**（如 3306） | **RESP** | **Cassandra 原生二进制协议 + CQL**；可选 **Alternator**（类 DynamoDB HTTP API） |
| **与 Scylla 原生兼容？** | **否**（非 MySQL 线协议） | **否**（非 RESP） | — |

**边界结论（协议）**

- ScyllaDB **不能**用 MySQL 客户端或 Redis 客户端直接当本机替换；迁移意味着 **换驱动、换查询语言/建模**（CQL 与表设计按分区键规划）。
- ScyllaDB 与 **Apache Cassandra** 生态（CQL、驱动）对齐；与 **DynamoDB** 的对齐通过 **Alternator** 等能力实现，仍与 MySQL/Redis 无关。

**举例场景（选型直觉）**

| 场景 | 更贴合的系统 | 简要理由 |
|------|----------------|----------|
| 订单、账务、库存强一致、多表 JOIN、复杂报表 SQL | **MySQL**（或 PostgreSQL 等 RDBMS） | 事务与 SQL 表达能力是主诉求；Scylla 不扮演通用 OLTP 关系库。 |
| 热点配置、登录 session、接口限流计数、短时去重 | **Redis** | 亚毫秒、内存结构、过期键；Scylla 不是默认缓存形态。 |
| 消息时间线、按用户 ID 分区的海量写入、IoT 时序宽表、风控事件流水 | **ScyllaDB**（或同类宽表库） | 分区键清晰、写放大可建模、多节点扩展；避免跨分区大扫描。（**展开见下**「时间线 / IoT / 风控」） |
| 用户资料 + 好友 Feed：资料要关系约束，Feed 要高并发写 | **MySQL + ScyllaDB**（组合） | 强一致小表走 RDBMS；Feed 按 user_id 分区走宽表；中间可用消息队列衔接。（**展开见下**「用户资料 + Feed」） |
| 已有 Cassandra 集群要更高吞吐/更低尾延迟 | **ScyllaDB** | 协议与 CQL 兼容思路一致，属于同类替换/迁移范畴，而非 MySQL 路线。 |
| 要兼容 AWS DynamoDB 应用代码 | **ScyllaDB + Alternator**（需配置） | HTTP API 对齐 DynamoDB；仍不是 Redis/MySQL。 |

#### 宽表是什么（为什么要和「分区键」一起理解）

在 **Cassandra / ScyllaDB** 语境里常说的 **宽表（wide-column）**，不是 MySQL 里「很多列拼成一张大表」的同义词，而是：

1. **先用 partition key（分区键）** 决定这一坨数据落在集群的哪一片、哪台节点上，保证 **同键上的读写局部化**。  
2. **在同一分区键内部**，再用 **聚簇列（clustering column）** 排顺序，形成 **很多行**（像「一串按时间排序的事件」），或在一行上挂 **多列**——对外查询通常是：**已知分区键** + **范围或等值**（例如「某用户最近 100 条」「某设备某时间段」）。

**举例 A（IoT / 时序）**  

- 逻辑：按设备写遥测。  
- 建模直觉：`PRIMARY KEY ((device_id), event_time)` —— `device_id` 是分区键；同一设备的事件落在同一分区，按 `event_time` 排序，**追加写入** 与 **按设备拉时间范围** 都自然。  
- **不适合**：没有 `device_id`、要扫全集群的「任意条件报表」——会变成分布式大扫描，应交给数仓/OLAP 或另行设计索引。

**举例 B（IM / 时间线）**  

- 逻辑：每个会话或每个收件箱一条时间线。  
- 建模直觉：分区键用 `conversation_id` 或 `inbox_user_id`；聚簇列用消息 ID / 时间，**一页页拉历史** 只触达少量分区。  
- **不适合**：「全站按关键字搜所有消息」这种与分区键无关的全文需求——需要 ES 等侧车索引，而不是单指望宽表主表。

**举例 C（风控流水）**  

- 逻辑：高 QPS 打点，事后按 `user_id` 或 `session_id` 拉近期行为链。  
- 建模直觉：分区键用 `user_id`（或哈希后的桶键以控制单分区体积），聚簇用 `ts`；写入只追加，读取按用户切片。

#### 场景重点展开：时间线 / 海量分区写入 / IoT / 风控

- **写入**：请求里 **天然带有** `user_id` / `device_id` / `session_id` 等分区键；每条事件 `INSERT` 落到固定分区，多节点 **并行扩展**，单分区过大时再 **加盐、分桶、按时间再分表** 等策略控制。  
- **读取**：产品路径多是「打开某会话」「某设备仪表盘」「某用户最近 N 条风控事件」——**都带着分区键**，延迟可控。  
- **反模式**：在宽表上频繁做 **不带分区键** 的扫表、或与分区布局冲突的二级索引滥用；会把 NoSQL 用成慢 SQL。

#### 场景重点展开：用户资料（MySQL）+ Feed（Scylla）

- **MySQL 管什么**：`users` 表（唯一邮箱、手机号、密码哈希、等级等）、好友关系、支付绑定等需要 **约束、事务、强一致** 的小表；查询模式是「按主键/唯一键点查、少量关联」。  
- **Scylla 管什么**：`posts_by_user` 一类表 —— `PRIMARY KEY ((user_id), post_id)` 或 `((user_id), created_at)`；发 Feed 时 **高并发追加** 一行；刷首页时 **带 `user_id` 分页拉取**，与上文「宽表」举例一致。  
- **怎么衔接**：用户改头像仍在 MySQL 更新；发帖可先写 Scylla，或通过 **Kafka / Pulsar 等** 异步落库；读 Profile 走 MySQL，读 Feed 走 Scylla。**不能**指望一条 MySQL 事务同时提交「好友表 + 全站 Feed 物理行」，那是两套一致性模型。

**一句话**：ScyllaDB 与 **Seastar** 是框架与产品关系；与 **MySQL / Redis** 是 **不同协议、不同模型** 的另一类系统——**互补或分层架构** 常见，**直接协议互换** 不成立。

### 13.2 典型特点

| 特点 | 说明 |
|------|------|
| **每核一线程（shared-nothing）** | 数据与任务尽量 **分片到固定 CPU 核**，减少跨核锁与缓存颠簸。 |
| **Reactor 模型** | 每个核一个 **事件循环**，统一处理 **网络、定时器、与本核绑定的任务**。 |
| **Future / continuation 风格** | 用 **延续（continuation）** 组织异步逻辑，思想与 **Tokio 的 Future** 相近，但 API 是 **C++** 的。 |
| **成套基础设施** | 除 TCP 外，还提供 **RPC、内存分配、指标** 等模块，目标是「搭分布式系统数据面」而非仅封装 `epoll`。 |

因此，Seastar **不是**「又一个通用小网络库」，而是偏 **重服务 / 数据面** 的框架；编程模型与约束（shard、跨核通信方式等）都比 **Thunder 当前「多进程 + libev + Step」** 更重。

### 13.3 有啥用处

**常见用途：**

- **ScyllaDB** 等 **NoSQL / 宽表数据库** 的数据库进程（Seastar 最典型的公开关联场景）。
- 其它需要 **多核打满、少锁、可预测延迟** 的 C++ 服务；有的项目 **全量基于 Seastar**，有的只借鉴其 **分片 + reactor** 思想。

**不适合作为默认首选的场景：**

- 「随便挂一个 HTTP 小服务、快速对接业务」——模型重、学习成本高。
- 与 **Thunder** 这种 **多进程 Manager/Worker、libev 事件线程、业务 Step 状态机** 的架构 **差异很大**；**不能**把 Seastar 当作 Thunder 的「直接替换运行时」而不做整体重写。

### 13.4 也是 io_uring 吗？

**不完全是。**

1. **存储（磁盘 I/O）路径**  
   Seastar 在 **磁盘异步 I/O** 上曾引入 / 支持 **io_uring 相关后端**（在 reactor 内对 **SQ/CQ 批处理** 那一套），用于 **异步读盘、写盘** 等场景。

2. **网络路径（两种常见形态）**  
   - **POSIX 栈**：走内核套接字 + reactor（如 epoll），与常见 Linux 网络服务类似。  
   - **Native 栈**（`--network-stack native`）：Seastar **自带的、按 shard 划分的 TCP/IP 实现**，文档说明 **通常与 [DPDK](https://github.com/scylladb/seastar/blob/master/doc/building-dpdk.md) 一起使用**，开发/测试也可用 **vhost** 等（见 [native-stack.md](https://github.com/scylladb/seastar/blob/master/doc/native-stack.md)）。  
   因此：**不是**「整个 Seastar = 纯 io_uring 网络栈」；io_uring 讨论主要落在 **存储** 侧，**网络高性能** 在不少部署里来自 **native + DPDK（用户态收发包路径）+ 框架本身**，而非 io_uring。

3. **稳定性与默认策略**  
   社区曾出现 **特定内核版本下 io_uring 不稳定** 的讨论，下游（如 Scylla、Redpanda 相关 PR/issue）也有 **默认关闭或回退 io_uring 后端** 的做法。说明在 Seastar 体系里，**io_uring 是「可选后端之一」**，落地必须带 **内核基线、测试与回退策略**（与本文 §6、§8 对 Thunder 迁移的提醒一致）。

### 13.5 一句话归纳

**Seastar = C++ 的高性能多核 Reactor 框架，主要面向数据库类数据面服务；io_uring 主要出现在其存储 I/O 路径上，并非「全程网络也用 io_uring」的代名词。**

### 13.6 DPDK、native 网络栈与「网络性能高」的关系

本节记录评审时的结论，避免把 Seastar 的性能 **单一归因** 为 DPDK 或 io_uring。

**官方文档要点**（[native-stack.md](https://github.com/scylladb/seastar/blob/master/doc/native-stack.md)）：

- Seastar 提供 **原生的、sharded 的 TCP/IP 协议栈**；启用方式为应用参数 **`--network-stack native`**。
- 该 native 栈 **通常与 DPDK 环境配合使用**；文档亦说明可在 **libvirt / virbr0 + tap（如 `scripts/tap.sh`）** 下做开发验证，而不必先上生产级 DPDK。

**DPDK 在这里的角色（为何常与「网络性能高」联系在一起）**

- DPDK 把大量 **收发包工作放在用户态、绕开内核协议栈的常规路径**，与 Seastar **自研 native 栈** 结合时，适合追求 **极高包处理吞吐、可控抖动** 的场景。
- 因此：在 **选用 native + DPDK** 的部署里，**DPDK + 用户态网络栈** 往往是 **网络侧特别猛的重要原因之一**。

**但并非「只有 DPDK」**

- Seastar 整体性能还来自 **shared-nothing、每核 Reactor、内存与调度设计** 等，与是否走 DPDK **无必然绑定**。
- 许多部署仍使用 **POSIX 网络栈**（不启用 native+DPDK），性能模型与运维成本不同。

**与 io_uring、Thunder 的对比口径**

- **io_uring**：在 Seastar 讨论中更常落在 **磁盘 I/O 后端**（见 §13.4），**不宜**与「native 网络栈 + DPDK」混为一谈。
- **Thunder**：当前为 **内核套接字 + libev**（或拟议 **Boost.Asio**）；**既不等于** Seastar native 栈，也 **不等于** DPDK 路径——若要对标 Seastar 极端网络性能，需 **单独评估** DPDK/用户态栈的引入成本与运维约束。

---

