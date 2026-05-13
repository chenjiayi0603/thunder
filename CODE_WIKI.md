# Thunder 框架 Code Wiki

## 1. 项目概述

**Thunder** 是一个基于 C++20 的分布式异步集群服务框架，提供 Center 注册发现、Worker 并发处理、HTTP 与内部二进制协议接入、可插拔模块（`.so`）等能力。

### 1.1 主要特性

- 基于事件驱动的异步网络模型，支持高并发连接处理
- 多进程 Worker 架构，支持插件动态加载（`Cmd*.so`、`Module*.so`）
- 支持 HTTP 编解码、内部二进制协议与多种编解码器扩展
- Center 集群支持 Raft 选主与主从语义下的注册/上报流程
- 内置 C++20 协程 Step 体系（`StepCo20`）与 Awaitable 能力
- 提供部署脚本、联调脚本、压测脚本和 Center 管理 CLI

---

## 2. 项目整体架构

### 2.1 目录结构

```
/workspace/
├── code/                     # 核心源码
│   ├── Net/                  # 网络库（核心框架）
│   ├── Center/               # 中心节点（注册发现）
│   ├── Hello/                 # 测试Demo节点
│   ├── Logic/                 # 逻辑服务器
│   ├── Interface/             # 登录/接口节点
│   ├── Proto/                 # 框架协议
│   ├── Util/                  # 框架通用库
│   ├── 3party/                # 第三方库
│   └── test/                  # 单元测试
├── deploy/                   # 部署产物、配置、脚本
├── docs/                     # 架构设计与专题文档
├── cmake/                    # CMake 配置
├── INSTALL.md               # 构建指南
└── README.md                 # 项目说明
```

### 2.2 核心模块架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                        Client (HTTP/WS/TLS)                      │
└───────────────────────────────┬─────────────────────────────────┘
                                │
┌───────────────────────────────▼─────────────────────────────────┐
│                      Hello/Interface Node                        │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    Manager Process                        │    │
│  │  - 信号处理 (SIGCHLD/SIGUSR1/SIGUSR2)                   │    │
│  │  - Worker 进程管理                                       │    │
│  │  - Center 注册/心跳                                      │    │
│  │  - 配置热更新                                            │    │
│  └─────────────────────────────────────────────────────────┘    │
│  ┌──────────┬──────────┬──────────┬──────────┐                   │
│  │ Worker 0 │ Worker 1 │ Worker 2 │ ... Worker N│              │
│  │ - IO事件  │ - IO事件  │ - IO事件  │  - IO事件   │              │
│  │ - Step   │ - Step   │ - Step   │  - Step    │              │
│  │ - Session│ - Session│ - Session│  - Session │              │
│  └──────────┴──────────┴──────────┴──────────┘                   │
│                              │                                   │
│  ┌───────────────────────────▼─────────────────────────────────┐│
│  │              Plugin System (Module*.so, Cmd*.so)            ││
│  │  - ModuleHello.so    - CmdGetToken.so                      ││
│  │  - ModuleInterface.so                                        ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Center Cluster                            │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐             │
│  │  Center 1   │   │  Center 2   │   │  Center 3   │             │
│  │  (Raft L)   │◄─►│  (F)       │◄─►│  (F)       │             │
│  └─────────────┘   └─────────────┘   └─────────────┘             │
│         │                                                      │
│         ▼                                                      │
│  ┌─────────────────────────────────────────────┐               │
│  │ SessionOnlineNodes - 在线节点管理             │               │
│  │ SessionRaftCluster - Raft 集群状态            │               │
│  └─────────────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. 核心模块详解

### 3.1 网络库 (Net)

网络库是 Thunder 框架的核心，位于 `code/Net/` 目录。

#### 3.1.1 关键目录结构

```
code/Net/
├── include/                    # 头文件
│   ├── Interface.hpp           # 框架对外接口
│   ├── labor/                  # Labor 相关
│   │   ├── Labor.hpp           # 抽象基类
│   │   ├── Manager.hpp         # 管理者
│   │   ├── Worker.hpp          # 工作进程
│   │   └── types/              # 数据类型定义
│   ├── session/                # 会话管理
│   │   └── Session.hpp         # 会话基类
│   ├── step/                  # 步骤/状态机
│   │   ├── Step.hpp           # 步骤基类
│   │   ├── RedisStep.hpp      # Redis步骤
│   │   ├── MysqlStep.hpp      # MySQL步骤
│   │   └── HttpStep.hpp        # HTTP步骤
│   ├── cmd/                    # 命令处理
│   │   ├── Cmd.hpp             # 命令基类
│   │   └── Module.hpp          # 模块基类
│   ├── codec/                  # 编解码器
│   │   ├── CodecCommon.hpp     # 通用定义
│   │   ├── HttpCodec.hpp       # HTTP编解码
│   │   ├── ThunderCodec.hpp    # 内部协议
│   │   └── *.hpp               # 其他编解码器
│   ├── coro/                   # C++20 协程
│   │   ├── Coroutine20.hpp     # 协程基础
│   │   ├── StepCo20.hpp        # 协程Step
│   │   └── Awaitable.hpp       # Awaitable
│   └── storage/                # 存储接口
│       ├── RedisOperator.hpp
│       └── DbOperator.hpp
└── src/                        # 实现文件
    ├── main.cpp                # 入口
    ├── labor/                  # Labor实现
    ├── session/                # 会话实现
    ├── step/                   # Step实现
    ├── cmd/                    # 命令实现
    └── storage/                # 存储实现
```

#### 3.1.2 Labor 体系

`Labor` 是框架层工作者的抽象基类，包括 `Manager` 和 `Worker`。

**关键文件**: [Labor.hpp](file:///workspace/code/Net/include/labor/Labor.hpp)

```cpp
namespace net
{
class Labor
{
public:
    Labor();
    virtual ~Labor();
    virtual bool Init(util::CJsonObject& oJsonConf);
    
    // 发送接口
    virtual bool SendTo(const tagMsgShell& stMsgShell) = 0;
    virtual bool SendTo(const tagMsgShell&, const MsgHead&, const MsgBody&) = 0;
    virtual bool AutoSend(const std::string& strIdentify, const MsgHead&, const MsgBody&) = 0;
    
    // 连接管理
    virtual bool SetConnectIdentify(const tagMsgShell&, const std::string&) = 0;
    virtual bool AddMsgShell(const std::string&, const tagMsgShell&) = 0;
    virtual void DelMsgShell(const std::string&, const tagMsgShell&) = 0;
    
    // 回调注册
    virtual bool RegisterCallback(Step*, double dTimeout = 0.0) = 0;
    virtual void DeleteCallback(Step*) = 0;
    virtual bool RegisterCallback(Session*) = 0;
    virtual void DeleteCallback(Session*) = 0;
    
    // 获取会话
    virtual Session* GetSession(uint64, const std::string& = "net::Session") = 0;
    virtual Session* GetSession(const std::string&, const std::string& = "net::Session") = 0;
};
}
```

### 3.2 Manager 进程

Manager 是主进程，负责整体协调和管理。

**关键文件**: [Manager.cpp](file:///workspace/code/Net/src/labor/Manager.cpp)

#### 3.2.1 主要职责

1. **进程管理**: 创建/监控/重启 Worker 子进程
2. **信号处理**: 处理 SIGCHLD、SIGUSR1、SIGUSR2 等信号
3. **Center 通信**: 向 Center 注册、心跳上报
4. **配置热更新**: 监听配置变化并通知 Worker
5. **负载均衡**: 将外部连接分配给 Worker

#### 3.2.2 核心流程

```cpp
Manager::Manager(const std::string& strConfFile)
{
    process_daemonize();           // 守护进程化
    InstallSignal();               // 安装信号处理
    CreateLoader();                // 创建配置加载进程
    LoadConf();                    // 加载配置
    SetProcessName();              // 设置进程名
    Init();                        // 初始化
    CreateEvents();                // 创建事件循环
    PreloadCmd();                  // 预加载命令
    CreateWorker();                // 创建Worker进程
    ReportToCenter();              // 向Center注册
}

void Manager::Run()
{
    ev_run(m_loop, 0);             // 进入事件循环
}
```

#### 3.2.3 信号处理

```cpp
// SIGCHLD: Worker 子进程退出
void Manager::OnChildTerminated(watcher) {
    while((iPid = waitpid(-1, &iStatus, WNOHANG)) > 0) {
        RestartWorker(iPid);       // 重启 Worker
    }
    ReportToCenter(true);          // 重新注册
}

// SIGUSR1: 刷新服务器配置
void Manager::RefreshServer() {
    if (LoadConf(boChanged)) {
        if (boChanged) {
            SendToWorker(CMD_REQ_SET_NODE_CUSTOM_CONFIG, ...);
        }
    }
}

// SIGUSR2: 重启所有 Worker
void Manager::RestartWorkers() {
    for (worker : m_mapWorker) {
        kill(worker.pid, SIGKILL);
    }
}
```

### 3.3 Worker 进程

Worker 是工作进程，负责实际的业务处理。

**关键文件**: [Worker.cpp](file:///workspace/code/Net/src/labor/Worker.cpp)

#### 3.3.1 主要职责

1. **IO 事件处理**: 读写网络数据
2. **业务逻辑执行**: 通过 Step/Module 执行
3. **会话管理**: 管理用户会话
4. **存储访问**: Redis/MySQL 等
5. **协程执行**: C++20 协程支持

#### 3.3.2 Worker 初始化流程

```cpp
Worker::Worker(const std::string& strWorkPath, int iControlFd, int iDataFd, 
               uint32 uiWorkerIndex, const util::CJsonObject& oConf)
{
    // 1. 保存 Manager 通信管道
    m_iControlFd = iControlFd;
    m_iDataFd = iDataFd;
    m_uiWorkerIndex = uiWorkerIndex;
    
    // 2. 加载插件
    LoadPlugins();
    
    // 3. 创建事件循环
    CreateEvents();
    
    // 4. 注册系统命令
    RegisterSysCmd();
    
    // 5. 初始化存储连接
    InitStorage();
}
```

### 3.4 Step 状态机

`Step` 是异步步骤基类，框架基于状态机设计模式。

**关键文件**: [Step.hpp](file:///workspace/code/Net/include/step/Step.hpp)

```cpp
namespace net
{
class Step
{
public:
    Step();
    Step(const tagMsgShell& stReqMsgShell);
    Step(const tagMsgShell&, const MsgHead&);
    Step(const tagMsgShell&, const MsgHead&, const MsgBody&);
    virtual ~Step();
    
    // 提交，发出
    virtual E_CMD_STATUS Emit(int iErrno = 0, 
                              const std::string& strErrMsg = "",
                              const std::string& strErrShow = "") = 0;
    
    // 步骤回调
    virtual E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                                 const MsgHead& oInMsgHead,
                                 const MsgBody& oInMsgBody,
                                 void* data = nullptr) = 0;
    
    // 超时回调
    virtual E_CMD_STATUS Timeout() = 0;
    
    // 注册/删除回调
    bool RegisterCallback(Step* pStep, ev_tstamp dTimeout = 0.0);
    void DeleteCallback(Step* pStep);
    
    // 请求上下文
    const tagMsgShell& GetReqMsgShell() const;
    const MsgHead& GetReqMsgHead() const;
    const MsgBody& GetReqMsgBody() const;
};
}
```

#### 3.4.1 Step 使用示例

```cpp
class MyBusinessStep : public net::Step
{
public:
    MyBusinessStep(const tagMsgShell& stMsgShell, 
                   const MsgHead& oMsgHead,
                   const MsgBody& oMsgBody)
        : net::Step(stMsgShell, oMsgHead, oMsgBody) {}
    
    E_CMD_STATUS Emit(int iErrno = 0, 
                      const std::string& strErrMsg = "",
                      const std::string& strErrShow = "") override
    {
        // 1. 发送请求到其他服务
        MsgHead oOutHead;
        MsgBody oOutBody;
        // ... 构建消息 ...
        
        // 2. 注册下一步回调
        auto pNextStep = new NextStep(...);
        RegisterCallback(pNextStep, 5.0);
        
        return STATUS_CMD_RUNNING;
    }
    
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                         const MsgHead& oInMsgHead,
                         const MsgBody& oInMsgBody,
                         void* data) override
    {
        // 处理响应
        // ...
        
        // 发送回复给客户端
        GetLabor()->SendToClient(m_stReqMsgShell, ...);
        
        return STATUS_CMD_COMPLETE;
    }
    
    E_CMD_STATUS Timeout() override
    {
        // 超时处理
        return STATUS_CMD_TIMEOUT;
    }
};

// 启动 Step
net::Launch(new MyBusinessStep(stMsgShell, oMsgHead, oMsgBody));
```

### 3.5 Session 会话管理

`Session` 用于存储跨请求的状态信息。

**关键文件**: [Session.hpp](file:///workspace/code/Net/include/session/Session.hpp)

```cpp
namespace net
{
class Session
{
public:
    Session(uint64 ulSessionId, 
            ev_tstamp dSessionTimeout = 60.0, 
            const std::string& strSessionClass = "net::Session");
    Session(const std::string& strSessionId,
            ev_tstamp dSessionTimeout = 60.0,
            const std::string& strSessionClass = "net::Session");
    virtual ~Session();
    
    // 超时回调
    virtual E_CMD_STATUS Timeout() = 0;
    
    // 初始化
    virtual bool Init(const util::CJsonObject& conf);
    
    // 永久会话（不因超时注销）
    void SetPermanent();
    bool IsPermanent() const;
};
}
```

#### 3.5.1 Session 使用示例

```cpp
class UserSession : public net::Session
{
public:
    static std::string SessionClass() { return "UserSession"; }
    
    UserSession(const std::string& strSessionId)
        : net::Session(strSessionId, 3600.0, SessionClass()) {}
    
    E_CMD_STATUS Timeout() override {
        LOG4_INFO("User session timeout: %s", GetSessionId().c_str());
        return STATUS_CMD_FINISH;
    }
    
    // 业务数据
    uint32_t user_id_;
    std::string token_;
};

// 创建会话
auto pSession = net::MakeSession<UserSession>(
    strSessionId, 
    UserSession::SessionClass(),
    3600.0
);

// 获取会话
auto pSession = GetLabor()->GetSession(sessionId, "UserSession");
```

### 3.6 Cmd 命令处理

`Cmd` 是命令处理基类，分为系统命令和业务命令。

**关键文件**: [Cmd.hpp](file:///workspace/code/Net/include/cmd/Cmd.hpp)

```cpp
namespace net
{
class Cmd
{
public:
    Cmd() = default;
    virtual ~Cmd() = default;
    
    // 初始化
    virtual bool Init() { return true; }
    
    // 处理任意消息
    virtual bool AnyMessage(const tagMsgShell& stMsgShell,
                            const MsgHead& oMsgHead,
                            const MsgBody& oMsgBody) = 0;
    
    void SetCmd(int iCmd) { m_iCmd = iCmd; }
    int GetCmd() const { return m_iCmd; }
    
protected:
    int m_iCmd = 0;
};
}
```

### 3.7 Module 模块

`Module` 用于动态加载的业务模块。

**关键文件**: [Module.hpp](file:///workspace/code/Net/include/cmd/Module.hpp)

```cpp
namespace net
{
class Module
{
public:
    Module() = default;
    virtual ~Module() = default;
    
    virtual bool Init() { return true; }
    virtual bool AnyMessage(const tagMsgShell& stMsgShell,
                           const MsgHead& oMsgHead,
                           const MsgBody& oMsgBody) = 0;
};
}
```

---

## 4. Center 节点

Center 是注册发现中心，支持 Raft 集群。

### 4.1 目录结构

```
code/Center/
├── src/
│   ├── CmdNodeRegister/        # 节点注册
│   ├── CmdNodeReport/         # 节点状态上报
│   ├── CmdNodeDisconnect/     # 节点断开
│   ├── CmdRaftRequestVote/    # Raft 投票请求
│   ├── CmdRaftAppendEntries/  # Raft 日志追加
│   ├── ModuleAdmin/           # 管理模块
│   ├── SessionOnlineNodes/    # 在线节点会话
│   └── SessionRaftCluster/    # Raft 集群会话
└── CMakeLists.txt
```

### 4.2 核心命令

#### 4.2.1 CmdNodeRegister - 节点注册

**关键文件**: [CmdNodeRegister.hpp](file:///workspace/code/Center/src/CmdNodeRegister/CmdNodeRegister.hpp)

```cpp
namespace coor
{
class CmdNodeRegister : public net::Cmd
{
public:
    CmdNodeRegister() = default;
    virtual ~CmdNodeRegister() = default;
    
    virtual bool Init();
    virtual bool AnyMessage(const net::tagMsgShell& stMsgShell,
                            const MsgHead& oMsgHead,
                            const MsgBody& oMsgBody) override;
                            
protected:
    bool InitFromDb(const util::CJsonObject& oDbConf);
    bool InitFromLocal(const util::CJsonObject& oLocalConf);
    
private:
    SessionOnlineNodes* m_pSessionOnlineNodes = nullptr;
};
}
```

#### 4.2.2 CmdNodeReport - 状态上报

节点定期向 Center 上报状态，Center 更新路由信息。

### 4.3 Raft 实现

Center 使用 Raft 协议实现高可用：

- **CmdRaftRequestVote**: 处理投票请求
- **CmdRaftAppendEntries**: 处理日志追加
- **SessionRaftCluster**: 管理 Raft 集群状态

---

## 5. 编解码器体系

### 5.1 编解码器架构

```
┌─────────────────────────────────────────────────────────────────┐
│                      外部协议 (Codec)                            │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐           │
│  │HttpCodec │ │Thunder   │ │WebSocket │ │  Proto   │           │
│  │          │ │Codec     │ │Json/Pb   │ │  Codec   │           │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘           │
│       │             │            │            │                  │
│       └─────────────┴────────────┴────────────┘                  │
│                            │                                    │
│                     ┌──────▼──────┐                             │
│                     │CodecFactory │                             │
│                     └──────┬──────┘                             │
│                            │                                    │
├────────────────────────────┼───────────────────────────────────┤
│                     内部协议 (MsgHead + MsgBody)                │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ MsgHead { cmd, seq, msgbody_len }                        │   │
│  │ MsgBody { body, req, rsp }                              │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Thunder 内部协议

**关键文件**: [CodecCommon.hpp](file:///workspace/code/Net/include/codec/CodecCommon.hpp)

```cpp
// 消息头结构 (12字节)
struct MsgHead
{
    uint32 cmd;           // 命令号
    uint32 seq;           // 序列号
    uint32 msgbody_len;   // 消息体长度
};

// 消息体结构
struct MsgBody
{
    string body;          // 业务数据
    string req;           // 请求标识
    string rsp;           // 响应标识
};
```

### 5.3 HTTP 编解码

**关键文件**: [HttpCodec.hpp](file:///workspace/code/Net/include/codec/HttpCodec.hpp)

支持标准 HTTP/1.1 协议，可扩展为 HTTPS。

---

## 6. C++20 协程支持

### 6.1 协程架构

框架使用 C++20 协程提供异步能力。

**关键文件**: [Coroutine20.hpp](file:///workspace/code/Net/include/coro/Coroutine20.hpp)

```cpp
namespace net
{
struct AsyncTask
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
    
    explicit AsyncTask(handle_type h);
    AsyncTask(AsyncTask&& other) noexcept;
    ~AsyncTask();
    
    struct promise_type
    {
        StepCo20* stepAutoNotify_{nullptr};
        
        template<typename... ExtraArgs>
        promise_type(StepCo20& step, ExtraArgs&&...) noexcept 
            : stepAutoNotify_(&step) {}
        
        AsyncTask get_return_object();
        std::suspend_never initial_suspend() noexcept;
        void return_void() noexcept;
        std::suspend_always final_suspend() noexcept;
    };
    
    handle_type native_handle() const noexcept;
    
private:
    handle_type coro_;
};
}
```

### 6.2 StepCo20 协程Step

**关键文件**: [StepCo20.hpp](file:///workspace/code/Net/include/coro/StepCo20.hpp)

提供协程形式的 Step，支持 `co_await` 异步操作。

```cpp
class StepCo20 : public Step
{
public:
    E_CMD_STATUS Callback(...) override;
    E_CMD_STATUS Timeout() override;
    
protected:
    virtual void OnCoroutineStart() {}
    virtual void OnCoroutineReturn() {}
};
```

---

## 7. 存储集成

### 7.1 Redis 操作

**关键文件**: [RedisOperator.hpp](file:///workspace/code/Net/include/storage/RedisOperator.hpp)

```cpp
class RedisOperator
{
public:
    bool Init(const util::CJsonObject& oConf);
    
    // 同步/异步命令
    bool Set(const std::string& key, const std::string& value);
    bool Get(const std::string& key, std::string& value);
    
    // 批量操作
    bool MSet(const std::map<std::string, std::string>& kvs);
    bool MGet(const std::vector<std::string>& keys, 
               std::vector<std::string>& values);
};
```

### 7.2 MySQL 操作

**关键文件**: [DbOperator.hpp](file:///workspace/code/Net/include/storage/DbOperator.hpp)

```cpp
class DbOperator
{
public:
    bool Init(const util::CJsonObject& oConf);
    
    // 查询接口
    bool Query(const std::string& sql);
    bool FetchRow(std::map<std::string, std::string>& row);
    
    // 事务支持
    bool Begin();
    bool Commit();
    bool Rollback();
};
```

---

## 8. 依赖关系

### 8.1 核心依赖

```
Thunder
├── Net (网络库)
│   ├── Util (工具库)
│   │   ├── log4cplus (日志)
│   │   ├── hiredis (Redis客户端)
│   │   ├── libcurl (HTTP客户端)
│   │   └── json (cJSON)
│   ├── Proto (协议库)
│   │   ├── protobuf
│   │   └── absl
│   └── 3party (第三方)
│       ├── libev (事件循环)
│       ├── OpenSSL (加密)
│       └── ...
├── Center (可选)
├── Logic (可选)
└── Interface (可选)
```

### 8.2 外部依赖

- **CMake**: >= 3.20
- **C++20**: GCC 10+ 或 Clang 12+
- **OpenSSL**: 1.1.0+
- **Protocol Buffers**: 3.x
- **libev**: 事件循环

---

## 9. 项目运行方式

### 9.1 编译构建

```bash
# 一键构建
git submodule update --init --recursive \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  && cmake --build build --target thirdparty_deploy -j1 \
  && cmake --build build -j1 \
  && cmake --install build
```

### 9.2 启动节点

```bash
# 使用统一脚本启动
cd deploy
./nodes.sh start all        # 启动所有节点
./nodes.sh stop all         # 停止所有节点
./nodes.sh restart all      # 重启所有节点
./nodes.sh restart Logic    # 重启 Logic 节点
./nodes.sh status           # 查看状态
```

### 9.3 配置说明

节点配置文件位于 `deploy/{NodeName}/conf/` 目录：

```json
{
    "server_name": "Hello",
    "node_type": "HELLO",
    "process_num": 4,
    "inner_host": "127.0.0.1",
    "inner_port": 27008,
    "access_host": "127.0.0.1",
    "access_port": 27008,
    "center": [
        {"host": "127.0.0.1", "port": 16068}
    ],
    "custom": {
        // 业务自定义配置
    },
    "io_timeout": 300,
    "log_level": "INFO"
}
```

### 9.4 测试

```bash
# 集成测试
python3 -m pytest deploy/tests/pytest -m "integration or smoke" --mode=local

# 性能测试
WRK_THREADS=4 WRK_CONNECTIONS=100 WRK_DURATION=10s \
  python3 -m pytest deploy/tests/pytest -m perf --mode=local -s
```

---

## 10. 插件开发

### 10.1 Module 插件

```cpp
// ModuleHello.cpp
#include "Module.hpp"

class ModuleHello : public net::Module
{
public:
    bool Init() override;
    bool AnyMessage(const net::tagMsgShell& stMsgShell,
                    const MsgHead& oMsgHead,
                    const MsgBody& oMsgBody) override;
};

// 导出函数
extern "C" ModuleHello* CreateModule()
{
    return new ModuleHello();
}

extern "C" void DestroyModule(ModuleHello* p)
{
    delete p;
}
```

### 10.2 Cmd 插件

```cpp
// CmdGetToken.cpp
#include "Cmd.hpp"

class CmdGetToken : public net::Cmd
{
public:
    bool Init() override;
    bool AnyMessage(const net::tagMsgShell& stMsgShell,
                    const MsgHead& oMsgHead,
                    const MsgBody& oMsgBody) override;
};
```

---

## 11. 总结

Thunder 是一个功能完整的分布式服务框架，核心设计包括：

1. **多进程架构**: Manager-Worker 模式，支持进程管理和热更新
2. **事件驱动**: 基于 libev 的高性能事件循环
3. **状态机设计**: Step 体系支持复杂的异步业务流程
4. **协程支持**: C++20 协程简化异步编程
5. **Raft 集群**: Center 节点高可用支持
6. **插件系统**: 动态加载业务模块
7. **多协议支持**: HTTP、WebSocket、内部二进制协议

---

*文档生成时间: 2026-05-13*
