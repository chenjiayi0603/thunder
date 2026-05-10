# Net 框架代码优化 & 端到端测试计划

> 目标：将 `code/Net` 源码库向现代 C++20 规范对齐，引入设计模式降低耦合，并补齐 E2E 核心功能测试。

---

## 一、现状诊断（Phase 1 探索结果）

### 1.1 代码规模
| 指标 | 数值 |
|------|------|
| 源文件总数 | 174（hpp/cpp/h，不含 .pb.* 生成代码） |
| 总代码行数 | ~65,920 |
| 最大文件 | Worker.cpp（5,006 行, 182KB） |
| 次大文件 | Manager.cpp（2,397 行, 85KB） |
| 大 WebSocket Codec | CodecWebSocketJson.cpp（1,479 行）、CodecWebSocketPb.cpp（1,452 行）、CodecWebSocketPbApp.cpp（1,448 行） |

### 1.2 不符合现代 C++ 规范的项

| 问题 | 数量 | 分布 |
|------|------|------|
| **C-style 转型** `(int)`, `(uint32)`, `(unsigned char*)` | ~45+ 处 | ProtoCodec.cpp, ThunderCodec.cpp, ConHash.hpp, Nodes.cpp |
| **裸 `new`/`delete`**（非 delete[]） | ~42 处 | Manager.cpp, Worker.cpp, 各存储 Operator, ConHash.hpp, Step 体系 |
| **`new[]` / `delete[]` 裸数组** | 6 处 | ThunderCodec.cpp（Rc5Encrypt/Rc5Decrypt, Zip 系列） |
| **`SAFE_DELETE` 宏** | 15 处 | 各存储 Operator, Step, Worker, Manager, Interface |
| **`void*` 参数** | ~12 处 | Step 体系, dispatcher/ConHash（一致性哈希）, libev 回调 |
| **裸 `free()`/`malloc()`** | ~8 处 | ThunderCodec.cpp（Zip/Unzip） |
| **static + C-cast 回调** | Worker/Manager 共 ~18 个 libev static 回调 | Worker.cpp, Manager.cpp |
| **全局单例指针** `g_pLabor` | 1 个 | Labor.cpp，全框架直接引用 |
| **匈牙利命名** | 广泛 | `oMsgHead`, `pBuff`, `m_strKey`, `uiModFactor`, `iFd` ... |
| **无操作函数** | 2 个 | Aes256Encrypt/Decrypt（标记 `//todo`, 仅赋值返回） |

### 1.3 设计模式现状

| 现有模式 | 位置 | 问题 |
|----------|------|------|
| **Strategy 模式**（编解码器） | `ThunderCodec` 及 8 个子类 | 编码器选择分散在 Worker/Manager 的 `if/else` 链中，未用注册表 |
| **Command 模式** | `Cmd` 基类 + 19 个子类 | 命令注册通过 `AddCmd(new Xxx(), CMD_XXX)` 手动调用 |
| **State Machine 模式**（Step） | `Step` 基类 + 6 个子类 | 裸指针传递，手动 register/unregister，无 RAII |
| **Facade 模式** | `Interface.cpp` | 函数松散，全局依赖 `GetLabor()` |
| **Template Method** | `Labor` 抽象基类 | `Manager` 和 `Worker` 各自实现但代码重复多 |
| **Singleton**（隐式） | `g_pLabor` 全局指针 | 无线程安全保护，测试中需手动替换 |

### 1.4 测试覆盖现状

| 已有测试（14 个文件, 154 个测试用例） | 覆盖模块 |
|------|------|
| `test_util_buffer` | CBuffer 环形缓冲区 |
| `test_util_json` | CJsonObject JSON 工具 |
| `test_util_threadpool` | ThreadPool 线程池 |
| `test_proto_coor` | 协议序列化（Raft/Node） |
| `test_codec_proto` | ProtoCodec 编解码 |
| `test_session` | Session 会话生命周期 |
| `test_connection` | ConnectionAttr 连接属性 |
| `test_storage_redis` | RedisOperator 存储协议 |
| `test_storage_mem` | MemOperator 存储协议 |
| `test_center_raft` | Raft 共识纯逻辑 |
| `test_coroutine20` | C++20 协程占位 |
| `test_step_co20` | StepCo20/HttpRespAwaiter |
| `test_net_interface` | Interface 工具函数（含 gmock） |
| `test_thunder_orm` | ORM（MysqlMapper/RedisMapper） |

**未覆盖的核心模块：**
- ❌ 所有 WebSocket Codec（3 个，共 4,379 行）
- ❌ HttpCodec / HttpsCodec
- ❌ AppMsgCodec / ClientMsgCodec / CodecCustom
- ❌ Dispatcher（ConHash 一致性哈希, Nodes 节点管理）
- ❌ Coro（RedisAwaitable, MySqlAwaitable, ThreadPoolAwaitable）
- ❌ Step（RedisStep, MysqlStep, HttpStep, StepNode）
- ❌ Labor / Loader / Logger
- ❌ Worker/Manager（框架核心，无法单独测试）
- ❌ E2E 集成测试（有 Docker Compose 但无自动化 GTest E2E）

---

## 二、优化计划

### Phase 2-1：现代 C++ 转型（3 天）

#### 2.1.1 C-style 转型 → C++ 转型
**优先级：高 | 风险：低**
- `ProtoCodec.cpp:95` `(int)(pBuff->ReadableBytes())` → `static_cast<int>(...)`
- `ThunderCodec.cpp` 全部 `(unsigned char*)` → `reinterpret_cast<unsigned char*>(...)`
- `Nodes.cpp` 全部 `(uint32)` → `static_cast<uint32>(...)`
- `dispatcher/ConHash.hpp:186` `void*` → 模板化或 `std::any`

#### 2.1.2 裸数组 → std::vector / std::string
**优先级：高 | 风险：低**
- `ThunderCodec.cpp` Rc5Encrypt/Rc5Decrypt 中 `new char[n]` → `std::vector<char>`
- `ThunderCodec.cpp` Zip/Unzip 中 `malloc`/`free` → `std::vector<Bytef>`
- `CodecWebSocketJson.cpp` base64 `new char[encodedMaxlen]` → `std::vector<char>`

#### 2.1.3 裸 new/delete → std::unique_ptr / std::shared_ptr
**优先级：中 | 风险：中**
- 存储 Operator（Redis/Mem/Db/Mongo）成员指针 → `std::unique_ptr`
- `Manager.cpp` 事件数据 `new tagManagerIoWatcherData` → `std::make_unique` + `release()`
- `Worker.cpp` 类似事件数据结构 → `std::unique_ptr`
- `ConHash.hpp` `new ConNode/ConVirtualNode` → `std::unique_ptr`

#### 2.1.4 SAFE_DELETE 宏移除
**优先级：高 | 风险：低**
- 用 `std::unique_ptr` 或显式 `delete` + `= nullptr` 替换所有 15 处 `SAFE_DELETE`
- 最终移除 `NetDefine.hpp` 中的宏定义

### Phase 2-2：设计模式改进（5 天）

#### 2.2.1 Codec 注册表（Strategy + Factory）
**优先级：高 | 风险：中 | 影响：代码整洁度大幅提升**
- **现状**：Worker 和 Manager 中各有一份 `if/else` 链（按 codec_type 选择具体 Codec）
- **方案**：实现 `CodecRegistry` 单例（或注入到 Labor），提供 `GetCodec(type)` 工厂方法
- **文件**：
  - 新建 `include/codec/CodecRegistry.hpp`
  - 新建 `src/codec/CodecRegistry.cpp`
  - Worker.cpp 和 Manager.cpp 中替换 if/else 链

```cpp
// CodecRegistry 伪代码
class CodecRegistry {
    std::unordered_map<util::E_CODEC_TYPE, std::function<std::unique_ptr<ThunderCodec>()>> factories_;
public:
    template<typename T>
    void Register(util::E_CODEC_TYPE type) {
        factories_[type] = [] { return std::make_unique<T>(); };
    }
    std::unique_ptr<ThunderCodec> Create(util::E_CODEC_TYPE type, const std::string& key = "");
};
```

#### 2.2.2 libev 回调 → 类型安全回调封装
**优先级：中 | 风险：高 | 影响：消除 void* 和 C-cast**
- **现状**：Worker 有 18 个 `static void Callback(struct ev_loop*, struct ev_*, int)` 函数，通过 `watcher->data` + C-cast 找回 `this`
- **方案**：为 `ev_io`、`ev_timer`、`ev_signal` 各创建一个 RAII 包装类，存储 `std::function<>` 回调
- **文件**：
  - 新建 `include/labor/EvWatcher.hpp`（`EvIoWatcher`, `EvTimerWatcher`, `EvSignalWatcher`）

```cpp
// EvIoWatcher 伪代码
class EvIoWatcher {
    ev_io watcher_;
    std::function<void(int revents)> callback_;
public:
    EvIoWatcher(struct ev_loop* loop, int fd, int events, std::function<void(int)> cb);
    ~EvIoWatcher();
    static void Trampoline(struct ev_loop*, ev_io*, int revents);
};
```

#### 2.2.3 Step 生命周期 RAII
**优先级：中 | 风险：中**
- **现状**：`Step*` 裸指针在 `RegisterCallback` 和 `DeleteCallback` 之间传递，调用方需手动管理
- **方案**：`RegisterCallback` 返回 `std::unique_ptr<Step>` 或 RAII guard；框架层统一管理 Step 生命周期
- **已有改进**：`Interface::Launch(std::unique_ptr<Step>)` 已用 `unique_ptr`——但内部 `pStep.release()` 后又回到裸指针
- 将 `Worker::ExecStep` 签名从 `Step*` 改为 `std::unique_ptr<Step>`，所有权完全转移

#### 2.2.4 Worker.cpp 拆分（单一职责）
**优先级：高 | 风险：高 | 影响：可维护性质的飞跃**
- **现状**：5,006 行单文件包含 IO、定时器、编解码、会话、Redis、命令分发所有逻辑
- **拆分方案**：
  - `WorkerIo.cpp`：IO 读写、连接管理（~1,200 行）
  - `WorkerTimer.cpp`：定时器/超时管理（~400 行）
  - `WorkerCodec.cpp`：编解码器选择与调用（~300 行）
  - `WorkerSession.cpp`：会话注册/超时（~300 行）
  - `WorkerRedis.cpp`：Redis 异步连接/回调（~800 行）
  - `WorkerCmd.cpp`：命令分发（~400 行）
  - `Worker.cpp`：核心构造/析构/事件循环（~600 行）

#### 2.2.5 依赖注入（降低全局状态耦合）
**优先级：中 | 风险：高**
- **现状**：所有模块通过 `GetLabor()` 全局函数获取当前 Labor 实例
- **方案**：Step/Cmd 基类持有 `Labor*` 引用（构造注入），而非通过全局函数
- 渐进式：先改为 `GetLabor()` 返回 thread_local 指针而非全局变量

### Phase 2-3：E2E 核心功能测试（4 天）

#### 2.3.1 Codec 完整编解码 E2E
**优先级：高 | 风险：低**
- 测试每种 Codec 的 Encode/Decode 往返：
  - `ProtoCodec` ✅ 已有
  - `HttpCodec`（HTTP 请求/响应解析）
  - `ClientMsgCodec`（客户端消息格式）
  - `AppMsgCodec`（应用层消息格式）
- 新增测试文件：`code/test/codec/test_codec_http.cpp`、`test_codec_client.cpp`

#### 2.3.2 Step 异步工作流 E2E
**优先级：高 | 风险：中**
- 使用 gmock 模拟 Worker，测试完整 Step 生命周期：
  - Step 创建 → RegisterCallback → Emit → Callback → Delete
  - Step 超时 → Timeout 回调
  - Step 链式串联（StepA.Callback 创建 StepB）
- 新增测试文件：`code/test/step/test_step_workflow.cpp`

#### 2.3.3 Dispatcher 一致性哈希 E2E
**优先级：中 | 风险：低**
- 测试一致性哈希环的节点添加、查询、虚拟节点分布
- 新增测试文件：`code/test/dispatcher/test_conhash.cpp`

#### 2.3.4 Coro Awaiter E2E
**优先级：中 | 风险：中**
- 测试 `RedisAwaitable`、`MySqlAwaitable` 的协程集成
- 测试 `ThreadPoolAwaitable` 在线程池执行后回事件循环
- 扩展 `test_step_co20.cpp`

#### 2.3.5 容器化集成测试（E2E Smoke）
**优先级：高 | 风险：中**
- 利用现有 `deploy/docker/docker-compose.yml`
- 编写 GTest 程序作为 HTTP 客户端，向已部署的 HelloHttp/HelloWs 发请求并验证响应
- 新增测试目录：`code/test/e2e/`
- 测试场景：
  - HelloHttp HTTP GET/POST → 验证 200 + JSON 响应
  - HelloWs WebSocket ping/pong
  - Center 注册节点 → 查询节点列表
  - Redis/MySQL 存储读写（通过 Hello 模块）

---

## 三、执行顺序与里程碑

### 里程碑 1：零风险转型（Day 1-2）
✅ C-style cast → C++ cast
✅ 裸数组 → std::vector
✅ SAFE_DELETE 移除
✅ 每步编译验证（`cmake --build build -j1`）+ 全部 154 个已有测试通过

### 里程碑 2：设计模式引入（Day 3-7）
✅ Codec 注册表（高收益低风险优先）
✅ Step 生命周期 RAII
✅ Worker.cpp 拆分
✅ EvWatcher RAII 封装（可选，风险高时可延后）

### 里程碑 3：测试补齐（Day 8-12）
✅ Codec E2E（HttpCodec, ClientMsgCodec, AppMsgCodec）
✅ Step 异步工作流
✅ Dispatcher 一致性哈希
✅ Coro Awaiter
✅ 容器化集成烟雾测试

---

## 四、风险与缓解

| 风险 | 缓解 |
|------|------|
| Worker.cpp 拆分导致编译依赖断裂 | 每次提取后立即 `cmake --build build -j1`；保留原 include 集合 |
| EvWatcher RAII 与 libev 生命周期不兼容 | 使用 `release()` 将所有权还给 libev 直至 watcher 销毁；或暂不做此项 |
| 依赖注入改动过大 | 保持 `GetLabor()` 接口不变，仅内部实现改为 thread_local；逐步迁移 |
| E2E 容器测试依赖 Docker 环境 | 使用 `THUNDER_E2E_ENABLED` 环境变量门控，CI 缺 Docker 时自动跳过 |
