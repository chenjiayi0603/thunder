# Thunder 框架完整 Code Wiki

> **项目版本**: Thunder Framework  
> **文档更新**: 2026-05-13  
> **框架特性**: C++20 分布式异步集群服务框架

---

## 目录

1. [项目概述](#1-项目概述)
2. [项目架构](#2-项目架构)
3. [核心模块详解](#3-核心模块详解)
4. [Manager进程详解](#4-manager进程详解)
5. [Worker进程详解](#5-worker进程详解)
6. [Step状态机机制](#6-step状态机机制)
7. [Session会话管理](#7-session会话管理)
8. [Cmd命令处理](#8-cmd命令处理)
9. [Module模块处理](#9-module模块处理)
10. [Center节点](#10-center节点)
11. [编解码器体系](#11-编解码器体系)
12. [协程支持](#12-协程支持)
13. [存储集成](#13-存储集成)
14. [依赖关系](#14-依赖关系)
15. [编译构建](#15-编译构建)
16. [部署运行](#16-部署运行)
17. [插件开发指南](#17-插件开发指南)

---

## 1. 项目概述

### 1.1 框架定位

Thunder 是一个基于 C++20 的**分布式异步集群服务框架**，面向"多节点、可扩展、可脚本化联调"的服务端场景。

### 1.2 核心能力

| 能力方向 | 具体实现 |
|---------|----------|
| **网络通信** | 基于 libev 的事件驱动异步网络模型，支持高并发连接 |
| **多进程架构** | Manager-Worker 模式，支持动态插件加载 |
| **协议支持** | HTTP、WebSocket、内部二进制协议、Protobuf |
| **服务发现** | Center 集群，支持 Raft 选主 |
| **异步编程** | C++20 协程 + Step 状态机 |
| **存储集成** | Redis、MySQL、MongoDB 等 |

### 1.3 目录结构

```
/workspace/
├── code/                          # 核心源码
│   ├── Net/                      # 网络库（核心框架）
│   │   ├── include/             # 头文件
│   │   │   ├── labor/           # Labor相关
│   │   │   ├── session/         # 会话
│   │   │   ├── step/            # 步骤
│   │   │   ├── cmd/             # 命令
│   │   │   ├── codec/           # 编解码
│   │   │   ├── coro/            # 协程
│   │   │   └── storage/         # 存储
│   │   └── src/                 # 实现
│   ├── Center/                   # 中心节点
│   │   └── src/                 # Center命令和会话
│   ├── Hello/                    # Demo节点
│   ├── Logic/                    # 逻辑服务器
│   ├── Interface/                # 接口节点
│   ├── Proto/                    # 协议定义
│   ├── Util/                     # 工具库
│   │   └── src/
│   │       ├── util/            # 通用工具
│   │       ├── dbi/             # 数据库接口
│   │       ├── curl/            # HTTP客户端
│   │       └── logging/         # 日志
│   ├── 3party/                   # 第三方库
│   └── test/                     # 测试代码
├── deploy/                       # 部署目录
│   ├── Center/                  # Center配置
│   ├── HelloHttp/               # HTTP服务
│   ├── HelloWs/                 # WebSocket
│   ├── HelloHttps/              # HTTPS服务
│   ├── Logic/                   # Logic配置
│   ├── Interface/               # Interface配置
│   ├── tests/                   # 测试脚本
│   └── nodes.sh                 # 启停脚本
├── docs/                        # 文档
├── cmake/                       # CMake配置
├── INSTALL.md                   # 构建指南
└── README.md                    # 项目说明
```

---

## 2. 项目架构

### 2.1 系统架构图

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              客户端层                                         │
│   ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐           │
│   │ HTTP    │  │  WS     │  │  WSS    │  │  HTTPS  │  │ TLS     │           │
│   │ Client  │  │ Client  │  │ Client  │  │ Client  │  │ Client  │           │
│   └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘           │
└────────┼───────────┼───────────┼───────────┼───────────┼───────────────────┘
         │           │           │           │           │
         ▼           ▼           ▼           ▼           ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                           接入层 (Hello/Interface)                             │
│  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │                          Manager 进程                                    │  │
│  │  • 信号处理 (SIGCHLD/SIGUSR1/SIGUSR2)                                  │  │
│  │  • Worker 进程管理                                                      │  │
│  │  • 外部连接监听                                                         │  │
│  │  • 负载均衡                                                             │  │
│  │  • Center 通信                                                          │  │
│  │  • 配置热更新                                                           │  │
│  └─────────────────────────────────────────────────────────────────────────┘  │
│                                    │                                          │
│  ┌─────────┬─────────┬─────────┬─────────┬─────────┐                        │
│  │Worker 0 │Worker 1 │Worker 2 │Worker 3 │Worker N │                        │
│  │ IO处理  │ IO处理  │ IO处理  │ IO处理  │ IO处理  │                        │
│  │ Step    │ Step    │ Step    │ Step    │ Step    │                        │
│  │ Session │ Session │ Session │ Session │ Session │                        │
│  └────┬────┴────┬────┴────┬────┴────┬────┴────┬────┘                        │
│       │         │         │         │         │                              │
│  ┌────▼─────────▼─────────▼─────────▼─────────▼────┐                        │
│  │              Plugin System (.so)                  │                        │
│  │  ModuleHello.so    CmdGetToken.so   ModuleInterface.so                    │
│  └───────────────────────────────────────────────────┘                        │
└──────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                          Center 集群 (Raft)                                    │
│                                                                              │
│    ┌───────────────┐    ┌───────────────┐    ┌───────────────┐               │
│    │  Center 1     │◄──►│  Center 2     │◄──►│  Center 3     │               │
│    │  (Leader)     │    │  (Follower)  │    │  (Follower)   │               │
│    └───────────────┘    └───────────────┘    └───────────────┘               │
│                                                                              │
│    核心功能:                                                                 │
│    • 节点注册/注销                                                           │
│    • 节点状态上报                                                            │
│    • 路由信息同步                                                            │
│    • Raft 选主                                                                │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                           存储层                                              │
│                                                                              │
│    ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐                       │
│    │  Redis  │  │  MySQL  │  │ MongoDB │  │ HBase   │                       │
│    └─────────┘  └─────────┘  └─────────┘  └─────────┘                       │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 进程间通信架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          Manager 进程                                    │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                      Unix Domain Socket Pair                     │   │
│  │   ControlFd[0] ◄──────────────────────────► ControlFd[1]       │   │
│  │   DataFd[0]    ◄──────────────────────────► DataFd[1]          │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                            │
└──────────────────────────────┼────────────────────────────────────────────┘
                               │
                    ┌──────────┴──────────┐
                    ▼                     ▼
         ┌────────────────┐      ┌────────────────┐
         │  Worker 0      │      │  Worker 1      │
         │  ControlFd[1]  │      │  ControlFd[1]  │
         │  DataFd[1]    │      │  DataFd[1]    │
         └────────────────┘      └────────────────┘
```

### 2.3 数据流架构

```
Client Request
      │
      ▼
┌─────────────────┐
│  协议解析        │  HttpCodec / ThunderCodec / WebSocketCodec
│  (Decode)       │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  路由分发        │  CmdRouter / ModuleRouter
│  (Dispatch)     │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  业务处理        │  Step / Session / Module
│  (Business)     │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  存储访问        │  RedisOperator / DbOperator
│  (Storage)      │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  响应封装        │  协议编码
│  (Encode)       │
└────────┬────────┘
         │
         ▼
   Client Response
```

---

## 3. 核心模块详解

### 3.1 模块依赖关系

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              main.cpp                                       │
│                           (程序入口)                                        │
└────────────────────────────────┬────────────────────────────────────────────┘
                                 ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Manager                                        │
│                           (父进程)                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ 信号处理      │  │ Worker管理   │  │ Center通信   │  │ 配置管理      │    │
│  │ Signal       │  │ Fork/Wait   │  │ 注册/心跳    │  │ 热更新       │    │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────────┘    │
└────────────────────────────────┬────────────────────────────────────────────┘
                                 │ fork()
                                 ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Worker                                         │
│                           (子进程)                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ 事件循环      │  │ 连接管理      │  │ Step调度     │  │ Session管理   │    │
│  │ libev        │  │ IO/Timeout   │  │ 状态机       │  │ 会话          │    │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────────┘    │
│                                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ 插件加载      │  │ 存储连接      │  │ 编解码        │  │ 协程         │    │
│  │ dlopen       │  │ Redis/MySQL │  │ Codec        │  │ Coroutine20  │    │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────────┘    │
└────────────────────────────────┬────────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           业务插件 (.so)                                     │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐          │
│  │ CmdHello         │  │ CmdGetToken      │  │ ModuleHello      │          │
│  │ 内部协议命令      │  │ Token获取        │  │ HTTP模块         │          │
│  └──────────────────┘  └──────────────────┘  └──────────────────┘          │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 核心类继承关系

```
                    ┌─────────────────┐
                    │    Labor        │  (抽象基类)
                    │ ─────────────── │
                    │ + SendTo()      │
                    │ + Register()    │
                    │ + GetSession()  │
                    └────────┬────────┘
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
          ▼                  ▼                  ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│    Manager      │  │    Worker       │  │    Loader      │
│   (父进程)      │  │   (子进程)      │  │  (配置加载)    │
└─────────────────┘  └─────────────────┘  └─────────────────┘


                    ┌─────────────────┐
                    │     Cmd         │  (命令基类)
                    │ ─────────────── │
                    │ + Init()        │
                    │ + AnyMessage()  │
                    └─────────────────┘
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
          ▼                  ▼                  ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│   CmdNodeRegister│  │ CmdRaftVote    │  │  CmdGetToken    │
│   (Center)      │  │  (Center)      │  │  (Logic)        │
└─────────────────┘  └─────────────────┘  └─────────────────┘


                    ┌─────────────────┐
                    │    Module       │  (模块基类)
                    │ ─────────────── │
                    │ + Init()        │
                    │ + AnyMessage()  │
                    └─────────────────┘
                             │
                             ▼
┌─────────────────┐  ┌─────────────────┐
│  ModuleHello    │  │ ModuleInterface │
│  (Hello)        │  │  (Interface)   │
└─────────────────┘  └─────────────────┘


                    ┌─────────────────┐
                    │     Step        │  (步骤基类)
                    │ ─────────────── │
                    │ + Emit()        │
                    │ + Callback()    │
                    │ + Timeout()     │
                    └─────────────────┘
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
          ▼                  ▼                  ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│   RedisStep    │  │   MysqlStep    │  │   HttpStep     │
│  (Redis操作)   │  │  (MySQL操作)   │  │  (HTTP请求)    │
└─────────────────┘  └─────────────────┘  └─────────────────┘


                    ┌─────────────────┐
                    │    Session      │  (会话基类)
                    │ ─────────────── │
                    │ + Timeout()     │
                    │ + Init()        │
                    └─────────────────┘
                             │
                             ▼
┌─────────────────┐  ┌─────────────────┐
│SessionOnlineNodes│ │ SessionRaft     │
│  (Center)       │  │  (Center)       │
└─────────────────┘  └─────────────────┘
```

---

## 4. Manager进程详解

### 4.1 职责概述

Manager 是 Thunder 框架的**主进程**，负责：

| 功能 | 描述 |
|------|------|
| **进程管理** | 创建、监控、重启 Worker 子进程 |
| **信号处理** | 处理 SIGCHLD、SIGUSR1、SIGUSR2、SIGTERM 等 |
| **Center 通信** | 向 Center 注册节点、心跳上报 |
| **配置热更新** | 监听配置文件变化，通知 Worker |
| **负载均衡** | 将外部连接分配给 Worker |
| **Unix Socket** | 与 Worker 通过 Unix Domain Socket 通信 |

### 4.2 核心文件

- **头文件**: [Manager.hpp](file:///workspace/code/Net/include/labor/Manager.hpp)
- **实现**: [Manager.cpp](file:///workspace/code/Net/src/labor/Manager.cpp)

### 4.3 初始化流程

```cpp
Manager::Manager(const std::string& strConfFile)
{
    // 1. 守护进程化
    process_daemonize(m_strWorkPath.c_str());
    
    // 2. 安装信号处理
    InstallSignal();
    
    // 3. 创建配置加载进程 (Loader)
    CreateLoader();
    
    // 4. 加载配置文件
    LoadConf(boChanged);
    
    // 5. 设置进程名
    SetProcessName(m_oCurrentConf);
    
    // 6. 初始化 (监听 Server 间通信端口)
    Init();
    
    // 7. 创建事件循环
    CreateEvents();
    
    // 8. 预加载系统命令
    PreloadCmd();
    
    // 9. 创建 Worker 进程
    CreateWorker();
    
    // 10. 向 Center 注册
    ReportToCenter();
}

void Manager::Run()
{
    ev_run(m_loop, 0);  // 进入事件循环
}
```

### 4.4 信号处理机制

```cpp
// Manager 支持的信号处理

// SIGCHLD: Worker 子进程退出
void Manager::OnChildTerminated(watcher)
{
    while((iPid = waitpid(-1, &iStatus, WNOHANG)) > 0)
    {
        RestartWorker(iPid);  // 重启该 Worker
    }
    ReportToCenter(true);     // 重新注册
}

// SIGUSR1: 刷新配置 (kill -SIGUSR1 <pid>)
void Manager::RefreshServer()
{
    if (LoadConf(boChanged))
    {
        if (boChanged)
        {
            SendToWorker(CMD_REQ_SET_NODE_CUSTOM_CONFIG, ...);  // 通知 Worker
        }
    }
}

// SIGUSR2: 重启所有 Worker (kill -SIGUSR2 <pid>)
void Manager::RestartWorkers()
{
    for (worker : m_mapWorker)
    {
        kill(worker.pid, SIGKILL);  // 杀死所有 Worker
    }
    // Manager 会收到 SIGCHLD 并重启 Worker
}

// SIGTERM/SIGINT: 优雅关闭
void Manager::OnManagerTerminated(watcher)
{
    ev_break(m_loop, EVBREAK_ALL);
    Destroy();
    exit(-1);
}
```

### 4.5 Worker 管理

```cpp
void Manager::CreateWorker()
{
    for (uint32 i = 0; i < m_uiWorkerNum; ++i)
    {
        // 创建 Unix Socket Pair
        socketpair(PF_UNIX, SOCK_STREAM, 0, iControlFds);
        socketpair(PF_UNIX, SOCK_STREAM, 0, iDataFds);
        
        iPid = fork();
        if (iPid == 0)  // 子进程
        {
            // 关闭父端 fd
            close(iControlFds[0]);
            close(iDataFds[0]);
            
            // 创建 Worker
            Worker* pWorker = new Worker(
                m_strWorkPath,
                iControlFds[1],
                iDataFds[1],
                i,
                m_oCurrentConf
            );
            pWorker->Run();
            delete pWorker;
            exit(-2);
        }
        else  // 父进程
        {
            // 保存 Worker 信息
            close(iControlFds[1]);
            close(iDataFds[1]);
            m_mapWorker[iPid] = {i, iControlFds[0], iDataFds[0]};
        }
    }
}

bool Manager::CheckWorker()
{
    for (worker : m_mapWorker)
    {
        if ((now - worker.dBeatTime) > m_iWorkerBeat)
        {
            kill(worker.pid, SIGKILL);  // Worker 无响应，杀死
            RestartWorker(worker.pid);  // 重启
        }
    }
}
```

### 4.6 Center 通信

```cpp
bool Manager::ReportToCenter(bool boRegister = false)
{
    NodeReport oNodeReport;
    oNodeReport.set_node_type(m_strNodeType);
    oNodeReport.set_node_id(m_uiNodeId);
    oNodeReport.set_node_ip(m_strHostForServer);
    oNodeReport.set_node_port(m_iPortForServer);
    oNodeReport.set_worker_num(m_mapWorker.size());
    
    // 收集各 Worker 负载
    for (worker : m_mapWorker)
    {
        oNodeReport.add_workers()->CopyFrom(worker.load);
    }
    
    if (boRegister)
        cmd = CMD_REQ_NODE_REGISTER;  // 注册
    else
        cmd = CMD_REQ_NODE_STATUS_REPORT;  // 状态上报
    
    // 发送到 Center
    AutoSend(strCenterIdentify, oMsgHead, oMsgBody);
}
```

### 4.7 数据处理流程

```cpp
bool Manager::RecvDataAndDispose(tagManagerIoWatcherData* pData, ...)
{
    // 1. 从 fd 读取数据
    iReadLen = pConn->pRecvBuff->ReadFD(pData->iFd, iErrno);
    
    // 2. 循环解析完整数据包
    while (pConn->pRecvBuff->ReadableBytes() >= MSG_HEAD_SIZE)
    {
        MsgHead oInMsgHead;
        oInMsgHead.ParseFromArray(buffer);
        
        MsgBody oInMsgBody;
        oInMsgBody.ParseFromArray(buffer + HEAD_SIZE, oInMsgHead.msgbody_len());
        
        // 3. 判断数据来源
        if (来自 Worker)
        {
            DisposeDataFromWorker(...);  // 处理 Worker 数据
        }
        else if (来自 Center)
        {
            DisposeDataFromCenter(...);   // 处理 Center 数据
        }
        else
        {
            DisposeDataAndTransferFd(...); // 转发给 Worker
        }
    }
}
```

---

## 5. Worker进程详解

### 5.1 职责概述

Worker 是 Thunder 框架的**工作进程**，每个 Worker 运行在独立的进程中：

| 功能 | 描述 |
|------|------|
| **IO 处理** | 处理客户端网络连接 |
| **业务逻辑** | 通过 Step/Module 执行 |
| **会话管理** | 管理用户会话状态 |
| **存储访问** | Redis/MySQL 操作 |
| **协程执行** | C++20 协程支持 |
| **插件加载** | 动态加载 .so 模块 |

### 5.2 核心文件

- **头文件**: [Worker.hpp](file:///workspace/code/Net/include/labor/Worker.hpp)
- **实现**: [Worker.cpp](file:///workspace/code/Net/src/labor/Worker.cpp)

### 5.3 初始化流程

```cpp
Worker::Worker(const std::string& strWorkPath,
               int iControlFd,
               int iDataFd,
               int iWorkerIndex,
               util::CJsonObject& oJsonConf)
{
    // 1. 保存 Manager 通信管道
    m_iControlFd = iControlFd;
    m_iDataFd = iDataFd;
    iWorkerIndex = iWorkerIndex;
    
    // 2. 加载配置
    SetConfFile(strConfFile);
    LoadConf();
    SetProcessName(m_oCurrentConf);
    
    // 3. 初始化 (连接 Redis/MySQL 等)
    Init(m_oCurrentConf);
    
    // 4. 创建事件循环
    CreateEvents();
    
    // 5. 初始化客户端监听
    InitClientListener();
    
    // 6. 加载插件
    LoadSo(m_oCurrentConf["cmd"]);
    LoadModule(m_oCurrentConf["module"]);
    
    // 7. 注册系统命令
    PreloadCmd();
}

void Worker::Run()
{
    ev_run(m_loop, 0);  // 进入事件循环
}
```

### 5.4 IO 事件处理

```cpp
// 读取数据
bool Worker::IoRead(tagIoWatcherData* pData, struct ev_io* watcher)
{
    auto pConn = mapFdAttr[pData->iFd].get();
    
    // 从 fd 读取到缓冲区
    int iReadLen = pConn->pRecvBuff->ReadFD(pData->iFd, iErrno);
    
    // 循环解析消息
    while (pConn->pRecvBuff->ReadableBytes() >= MSG_HEAD_SIZE)
    {
        // 解析消息头
        MsgHead oInMsgHead;
        oInMsgHead.ParseFromArray(pConn->pRecvBuff->GetRawReadBuffer());
        
        // 解析消息体
        if (pConn->pRecvBuff->ReadableBytes() >= HEAD_SIZE + oInMsgHead.msgbody_len())
        {
            MsgBody oInMsgBody;
            oInMsgBody.ParseFromArray(...);
            
            // 交给 Codec/Command 处理
            pConn->pCodec->Decode(pConn, oInMsgHead, oInMsgBody);
        }
    }
}

// 写数据
bool Worker::IoWrite(tagIoWatcherData* pData, struct ev_io* watcher)
{
    auto pConn = mapFdAttr[pData->iFd].get();
    
    // 从发送缓冲区写到 fd
    int iWriteLen = pConn->pSendBuff->WriteFD(pData->iFd, iErrno);
    
    if (iWriteLen == pConn->pSendBuff->ReadableBytes())
    {
        RemoveIoWriteEvent(pConn);  // 发送完成，关闭写事件
    }
}
```

### 5.5 消息分发

```cpp
bool Worker::Dispose(const tagConnectionAttr* pConn,
                     const MsgHead& oInMsgHead,
                     const MsgBody& oInMsgBody,
                     MsgHead& oOutMsgHead,
                     MsgBody& oOutMsgBody)
{
    uint32 uiCmd = gc_uiCmdBit & oInMsgHead.cmd();
    
    // 1. 查找内置命令
    auto cmd_iter = m_mapCmd.find(uiCmd);
    if (cmd_iter != m_mapCmd.end())
    {
        cmd_iter->second->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
        return true;
    }
    
    // 2. 查找动态加载的命令
    auto so_iter = m_mapCmdFromSo.find(uiCmd);
    if (so_iter != m_mapCmdFromSo.end())
    {
        so_iter->second->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
        return true;
    }
    
    // 3. 查找模块 (HTTP)
    if (pConn->eCodec == CODEC_HTTP)
    {
        auto module_iter = m_mapModule.find(url_path);
        if (module_iter != m_mapModule.end())
        {
            module_iter->second->AnyMessage(stMsgShell, oHttpMsg);
            return true;
        }
    }
    
    return false;
}
```

### 5.6 插件加载

```cpp
void Worker::LoadSo(util::CJsonObject& oSoConf, bool boForce = false)
{
    for (int i = 0; i < oSoConf.GetArraySize(); ++i)
    {
        std::string strSoPath = oSoConf[i]("so");
        std::string strSymbol = oSoConf[i]("entry");
        int iVersion = oSoConf[i]("version", 1);
        
        // 加载动态库
        void* handle = dlopen(strSoPath.c_str(), RTLD_NOW);
        if (!handle)
        {
            LOG4_ERROR("dlopen %s error: %s", strSoPath.c_str(), dlerror());
            continue;
        }
        
        // 获取创建/销毁函数
        auto Create = (Cmd* (*)())dlsym(handle, strSymbol.c_str());
        auto Destroy = (void (*)(Cmd*))dlsym(handle, "Destroy");
        
        // 创建实例
        Cmd* pCmd = Create();
        AddCmd(pCmd, oSoConf[i]("cmd"));
        
        // 保存句柄
        m_mapSoHandle[pCmd] = handle;
    }
}

void Worker::LoadModule(util::CJsonObject& oModuleConf, bool boForce = false)
{
    // 类似于 LoadSo，但加载 Module
    for (int i = 0; i < oModuleConf.GetArraySize(); ++i)
    {
        std::string strSoPath = oModuleConf[i]("so");
        std::string strSymbol = oModuleConf[i]("entry");
        
        void* handle = dlopen(strSoPath.c_str(), RTLD_NOW);
        auto Create = (Module* (*)())dlsym(handle, strSymbol.c_str());
        
        Module* pModule = Create();
        std::string strUrlPath = oModuleConf[i]("url_path");
        m_mapModule[strUrlPath] = pModule;
    }
}
```

---

## 6. Step状态机机制

### 6.1 设计理念

Thunder 框架采用**状态机设计模式**处理异步业务逻辑。Step 是状态机的基本单位。

### 6.2 核心文件

- **头文件**: [Step.hpp](file:///workspace/code/Net/include/step/Step.hpp)

### 6.3 Step 类结构

```cpp
namespace net
{
class Step
{
public:
    // 构造方式
    Step();                                          // 无参构造
    Step(const tagMsgShell& stReqMsgShell);          // 指定请求源
    Step(const tagMsgShell&, const MsgHead&);         // 指定请求头
    Step(const tagMsgShell&, const MsgHead&, const MsgBody&);  // 完整请求
    
    virtual ~Step();
    
    // 三个核心方法
    virtual E_CMD_STATUS Emit(...) = 0;      // 发出请求
    virtual E_CMD_STATUS Callback(...) = 0;  // 回调处理
    virtual E_CMD_STATUS Timeout() = 0;      // 超时处理
    
    // 注册/删除回调
    bool RegisterCallback(Step* pStep, ev_tstamp dTimeout = 0.0);
    void DeleteCallback(Step* pStep);
    
    // 获取请求上下文
    const tagMsgShell& GetReqMsgShell() const;
    const MsgHead& GetReqMsgHead() const;
    const MsgBody& GetReqMsgBody() const;
    
    // Step 标识
    uint32 GetSequence();                    // 获取 Step 序列号
    const std::string& ClassName() const;    // 获取类名
    
protected:
    // 请求上下文
    tagMsgShell m_stReqMsgShell;              // 请求来源
    MsgHead m_oReqMsgHead;                    // 请求头
    MsgBody m_oReqMsgBody;                    // 请求体
    HttpMsg m_oInHttpMsg;                     // HTTP 请求
    
    // 定时器
    ev_timer* m_pTimeoutWatcher = nullptr;
    ev_tstamp m_dTimeout = 0.5;
    
private:
    bool m_bRegistered = false;
    uint32 m_ulSequence = 0;
};
}
```

### 6.4 状态返回值

```cpp
enum E_CMD_STATUS
{
    STATUS_CMD_RUNNING = 0,    // 继续运行，等待回调
    STATUS_CMD_COMPLETE = 1,   // 完成，Step 将被销毁
    STATUS_CMD_TIMEOUT = 2,    // 超时，Step 将被销毁
    STATUS_CMD_PAUSE = 3,      // 暂停（保留）
};
```

### 6.5 使用示例

#### 6.5.1 简单业务 Step

```cpp
class GetUserInfoStep : public net::Step
{
public:
    GetUserInfoStep(const tagMsgShell& stMsgShell,
                    const MsgHead& oMsgHead,
                    const MsgBody& oMsgBody,
                    uint32 uiUserId)
        : net::Step(stMsgShell, oMsgHead, oMsgBody)
        , m_uiUserId(uiUserId) {}
    
    E_CMD_STATUS Emit(int iErrno = 0,
                     const std::string& strErrMsg = "",
                     const std::string& strErrShow = "") override
    {
        // 1. 构建 Redis 查询命令
        auto pRedisStep = new GetUserFromRedisStep(
            m_stReqMsgShell, m_oReqMsgHead, m_oReqMsgBody, m_uiUserId);
        
        // 2. 注册回调
        if (!RegisterCallback(pRedisStep, 5.0))
        {
            delete pRedisStep;
            return STATUS_CMD_TIMEOUT;
        }
        
        // 3. 发送请求
        std::string strCmd = "HGETALL user:" + std::to_string(m_uiUserId);
        GetLabor()->AutoRedisCmd("127.0.0.1", 6379, pRedisStep);
        
        return STATUS_CMD_RUNNING;
    }
    
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                         const MsgHead& oInMsgHead,
                         const MsgBody& oInMsgBody,
                         void* data) override
    {
        // 解析 Redis 响应
        // ...
        
        // 发送响应给客户端
        MsgBody oRspBody;
        oRspBody.set_body(response_json);
        GetLabor()->SendToClient(m_stReqMsgShell, oInMsgHead, oRspBody);
        
        return STATUS_CMD_COMPLETE;
    }
    
    E_CMD_STATUS Timeout() override
    {
        LOG4_WARN("GetUserInfoStep timeout for user %u", m_uiUserId);
        
        MsgBody oRspBody;
        oRspBody.mutable_ret()->set_code(ETIMEDOUT);
        GetLabor()->SendToClient(m_stReqMsgShell, m_oReqMsgHead, oRspBody);
        
        return STATUS_CMD_TIMEOUT;
    }
    
private:
    uint32 m_uiUserId;
};
```

#### 6.5.2 Redis 操作 Step

```cpp
class GetUserFromRedisStep : public net::RedisStep
{
public:
    GetUserFromRedisStep(const tagMsgShell& stMsgShell,
                         const MsgHead& oMsgHead,
                         const MsgBody& oMsgBody,
                         uint32 uiUserId)
        : net::RedisStep(stMsgShell, oMsgHead, oMsgBody)
        , m_uiUserId(uiUserId) {}
    
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                         const MsgHead& oInMsgHead,
                         const MsgBody& oInMsgBody,
                         void* data) override
    {
        // 获取 Redis 回复
        redisReply* reply = static_cast<redisReply*>(data);
        
        if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY)
        {
            // Redis 错误，查询数据库
            auto pDbStep = new GetUserFromDbStep(
                GetReqMsgShell(), GetReqMsgHead(), GetReqMsgBody(), m_uiUserId);
            
            RegisterCallback(pDbStep, 5.0);
            GetLabor()->AutoMysqlCmd(pDbStep);
            
            return STATUS_CMD_RUNNING;
        }
        
        // 解析用户信息
        // ...
        
        // 创建下一步
        auto pRespStep = new ResponseUserInfoStep(
            GetReqMsgShell(), GetReqMsgHead(), GetReqMsgBody(), user_info);
        
        return pRespStep->Emit();
    }
    
private:
    uint32 m_uiUserId;
};
```

#### 6.5.3 链式调用

```cpp
// 业务处理流程：验证 -> 查询 -> 响应
class VerifyTokenStep : public net::Step { /* ... */ };
class QueryDataStep : public net::Step { /* ... */ };
class ResponseStep : public net::Step { /* ... */ };

// 业务入口
void ProcessBusiness(const tagMsgShell& stMsgShell,
                     const MsgHead& oMsgHead,
                     const MsgBody& oMsgBody)
{
    auto pVerifyStep = new VerifyTokenStep(stMsgShell, oMsgHead, oMsgBody);
    
    if (!net::Launch(pVerifyStep, 3, 1, 5.0))
    {
        // 启动失败
        delete pVerifyStep;
    }
}

// VerifyTokenStep::Callback
E_CMD_STATUS VerifyTokenStep::Callback(...)
{
    if (token_valid)
    {
        // token 有效，查询数据
        auto pQueryStep = new QueryDataStep(GetReqMsgShell(), ...);
        RegisterCallback(pQueryStep, 5.0);
        // 发起查询...
        return STATUS_CMD_RUNNING;
    }
    else
    {
        // token 无效，直接响应错误
        auto pRespStep = new ResponseStep(
            GetReqMsgShell(), GetReqMsgHead(), GetReqMsgBody(), 
            ERR_TOKEN_INVALID, "Token无效");
        return pRespStep->Emit();
    }
}

// QueryDataStep::Callback
E_CMD_STATUS QueryDataStep::Callback(...)
{
    // 数据查询完成，响应
    auto pRespStep = new ResponseStep(
        GetReqMsgShell(), GetReqMsgHead(), GetReqMsgBody(),
        ERR_OK, "成功", query_result);
    return pRespStep->Emit();
}
```

### 6.6 Step 生命周期

```
┌─────────────┐
│   创建      │
│  new Step  │
└─────┬──────┘
      │
      ▼
┌─────────────┐
│   Emit()   │──────► 发送请求，注册回调
│  (发出)     │         返回 STATUS_CMD_RUNNING
└─────┬──────┘
      │
      │ 等待回调/超时
      │
      ├─────────────────┬─────────────────┐
      ▼                 ▼                 ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│  Callback() │  │  Callback() │  │  Timeout() │
│  (成功)     │  │  (失败)     │  │  (超时)     │
└─────┬──────┘  └─────┬──────┘  └─────┬──────┘
      │                 │                 │
      ▼                 ▼                 ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│ Emit(下一步)│  │ Emit(错误)  │  │ 错误响应    │
│ 或 COMPLETE │  │ 或 COMPLETE │  │ 或 COMPLETE │
└─────┬──────┘  └─────┬──────┘  └─────┬──────┘
      │                 │                 │
      └────────┬────────┴────────┬────────┘
               │                 │
               ▼                 ▼
        ┌─────────────┐  ┌─────────────┐
        │   完成      │  │   销毁      │
        │  业务完成   │  │ delete this │
        └─────────────┘  └─────────────┘
```

---

## 7. Session会话管理

### 7.1 设计目的

Session 用于存储**跨请求的持久状态**，与 Step 的区别：

| 特性 | Step | Session |
|------|------|---------|
| 生命周期 | 单次请求 | 多请求持久 |
| 用途 | 请求处理链 | 状态保持 |
| 超时 | 短时 (秒级) | 长时 (分钟/小时) |
| 创建 | 每个请求创建 | 按需创建 |

### 7.2 核心文件

- **头文件**: [Session.hpp](file:///workspace/code/Net/include/session/Session.hpp)

### 7.3 Session 类结构

```cpp
namespace net
{
class Session
{
public:
    // 构造方式
    Session(uint64 ulSessionId,
            ev_tstamp dSessionTimeout = 60.0,
            const std::string& strSessionClass = "net::Session");
    
    Session(const std::string& strSessionId,
            ev_tstamp dSessionTimeout = 60.0,
            const std::string& strSessionClass = "net::Session");
    
    virtual ~Session();
    
    // 超时回调
    virtual E_CMD_STATUS Timeout() = 0;
    
    // 初始化 (可选)
    virtual bool Init(const util::CJsonObject& conf) { return true; }
    
    // 获取标识
    const std::string& GetSessionId() const;
    const std::string& GetSessionClass() const;
    
    // 永久会话
    void SetPermanent();        // 设置为永久会话
    bool IsPermanent() const;   // 是否永久
    
    // 获取/设置超时
    ev_tstamp GetTimeout() const;
    ev_tstamp GetActiveTime() const;
    
protected:
    void SetActiveTime(ev_tstamp activeTime);  // 更新活跃时间
    bool IsRegistered() const;                  // 是否已注册
    
private:
    void SetRegistered();
    
private:
    ev_tstamp m_dSessionTimeout = 60.0;        // 超时时间
    bool m_bRegistered = false;                // 注册状态
    ev_tstamp m_activeTime = 0;                 // 活跃时间
    std::string m_strSessionId;                 // 会话ID
    std::string m_strSessionClassName;          // 会话类名
    ev_timer* m_pTimeoutWatcher = nullptr;     // 定时器
    bool m_boPermanent = false;                 // 永久会话
    
    friend class Labor;
    friend class Worker;
    friend class Manager;
};
}
```

### 7.4 使用示例

#### 7.4.1 用户会话

```cpp
class UserSession : public net::Session
{
public:
    static std::string ClassType() { return "UserSession"; }
    
    UserSession(const std::string& strSessionId,
                ev_tstamp dTimeout = 3600.0)
        : net::Session(strSessionId, dTimeout, ClassType()) {}
    
    E_CMD_STATUS Timeout() override
    {
        LOG4_INFO("User session timeout: %s", GetSessionId().c_str());
        
        // 清理资源
        //Logout();
        
        return STATUS_CMD_FINISH;  // 会话结束，销毁
    }
    
    // 业务数据
    uint32_t user_id_ = 0;
    std::string token_;
    std::string nickname_;
    time_t login_time_;
    std::vector<uint32_t> friend_list_;
    
    // 业务方法
    void AddFriend(uint32_t friend_id)
    {
        friend_list_.push_back(friend_id);
        SetActiveTime(ev_now(GetLabor()->GetEvLoop()));  // 更新活跃时间
    }
};

// 创建会话
UserSession* pSession = net::MakeSession<UserSession>(
    strSessionId,                    // 会话ID
    UserSession::ClassType(),         // 会话类型
    3600.0,                          // 超时时间
    util::CJsonObject()               // 配置
);

// 获取会话
UserSession* pSession = dynamic_cast<UserSession*>(
    GetLabor()->GetSession(strSessionId, "UserSession"));

if (pSession)
{
    pSession->AddFriend(friend_id);
}
```

#### 7.4.2 全局配置会话

```cpp
// 全局配置会话模板
template <typename T>
T* GetGlobalConfigSession(const std::string& configFileName = "",
                         ev_tstamp dSessionTimeout = 1.0)
{
    static T* g_pSession = nullptr;
    if (g_pSession) return g_pSession;
    
    util::CJsonObject oConf;
    if (!configFileName.empty())
    {
        net::GetConfig(oConf, net::GetConfigPath() + configFileName);
    }
    
    return (g_pSession = net::MakeSession<T>(
        T::ClassType(), T::ClassType(), dSessionTimeout, oConf));
}

// 使用
class RedisConfigSession : public net::Session
{
public:
    static std::string ClassType() { return "RedisConfigSession"; }
    
    RedisConfigSession(const std::string& id, ev_tstamp timeout,
                       const util::CJsonObject& conf)
        : net::Session(id, timeout, ClassType())
    {
        Init(conf);
    }
    
    E_CMD_STATUS Timeout() override { return STATUS_CMD_FINISH; }
    
    bool Init(const util::CJsonObject& conf) override
    {
        m_strHost = conf("host");
        m_iPort = conf("port", 6379);
        return true;
    }
    
    std::string m_strHost;
    int m_iPort;
};

// 获取全局 Redis 配置
RedisConfigSession* pRedisConf = GetGlobalConfigSession<RedisConfigSession>(
    "redis.json", 10.0);
```

### 7.5 Session 与 Step 交互

```cpp
// 业务处理中创建 Session
class LoginStep : public net::Step
{
public:
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                         const MsgHead& oInMsgHead,
                         const MsgBody& oInMsgBody,
                         void* data) override
    {
        // 验证成功，创建用户会话
        auto pSession = net::MakeSession<UserSession>(
            strSessionId, UserSession::ClassType(), 3600.0);
        
        if (pSession)
        {
            pSession->user_id_ = user_info.id;
            pSession->token_ = GenerateToken();
            pSession->nickname_ = user_info.nickname;
            
            // 响应客户端
            MsgBody oRsp;
            oRsp.set_session_id(strSessionId);
            oRsp.set_token(pSession->token_);
            SendToClient(GetReqMsgShell(), GetReqMsgHead(), oRsp);
        }
        
        return STATUS_CMD_COMPLETE;
    }
};

// 其他 Step 中使用 Session
class QueryFriendStep : public net::Step
{
public:
    E_CMD_STATUS Emit(...) override
    {
        // 获取用户会话
        auto pSession = GetLabor()->GetSession(session_id, "UserSession");
        if (!pSession)
        {
            return Emit(ERR_SESSION_NOT_EXIST, "会话不存在");
        }
        
        auto pUserSession = dynamic_cast<UserSession*>(pSession);
        uint32_t user_id = pUserSession->user_id_;
        
        // 查询好友列表
        return QueryFriendList(user_id);
    }
};
```

---

## 8. Cmd命令处理

### 8.1 设计目的

Cmd 是**命令处理基类**，用于处理内部二进制协议消息。

### 8.2 核心文件

- **头文件**: [Cmd.hpp](file:///workspace/code/Net/include/cmd/Cmd.hpp)

### 8.3 Cmd 类结构

```cpp
namespace net
{
class Cmd
{
public:
    Cmd();
    virtual ~Cmd();
    
    // 初始化 (可选)
    virtual bool Init() { return true; }
    
    // 命令处理入口
    virtual bool AnyMessage(const tagMsgShell& stMsgShell,
                           const MsgHead& oMsgHead,
                           const MsgBody& oMsgBody) = 0;
    
    // 获取/设置命令号
    int GetCmd() const;
    void SetCmd(int iCmd);
    
    // 获取类名
    const std::string& ClassName() const;
    
protected:
    void SetClassName(const std::string& strClassName);
    
protected:
    char m_pErrBuff[gc_iErrBuffLen];
    uint32 m_uiCmd = 0;
    
private:
    std::string m_strClassName;
};
}
```

### 8.4 使用示例

```cpp
// 定义命令
class CmdGetToken : public net::Cmd
{
public:
    CmdGetToken() { SetClassName("CmdGetToken"); }
    
    bool Init() override
    {
        // 加载配置
        return net::GetConfig(m_oConf, net::GetConfigPath() + "Logic.json");
    }
    
    bool AnyMessage(const tagMsgShell& stMsgShell,
                   const MsgHead& oMsgHead,
                   const MsgBody& oMsgBody) override
    {
        // 解析请求
        GetTokenReq oReq;
        if (!oReq.ParseFromString(oMsgBody.body()))
        {
            // 响应错误
            SendErrorRsp(stMsgShell, oMsgHead, ERR_PARASE_PROTOBUF);
            return true;
        }
        
        // 验证参数
        if (oReq.username().empty() || oReq.password().empty())
        {
            SendErrorRsp(stMsgShell, oMsgHead, ERR_INVALID_PARAM);
            return true;
        }
        
        // 创建业务 Step 处理
        auto pStep = new GetTokenStep(stMsgShell, oMsgHead, oMsgBody,
                                     oReq.username(), oReq.password());
        return net::Launch(pStep, 3, 1, 5.0);
    }
    
private:
    void SendErrorRsp(const tagMsgShell& stMsgShell,
                     const MsgHead& oMsgHead,
                     int iErrCode)
    {
        GetTokenRsp oRsp;
        oRsp.set_err_code(iErrCode);
        
        MsgBody oMsgBody;
        oMsgBody.set_body(oRsp.SerializeAsString());
        
        GetLabor()->SendTo(stMsgShell, oMsgHead.cmd() + 1,
                          oMsgHead.seq(), oMsgBody.body());
    }
    
    util::CJsonObject m_oConf;
};
```

---

## 9. Module模块处理

### 9.1 设计目的

Module 用于处理 **HTTP/HTTPS 请求**，类似于 Web 框架的路由控制器。

### 9.2 核心文件

- **头文件**: [Module.hpp](file:///workspace/code/Net/include/cmd/Module.hpp)

### 9.3 Module 类结构

```cpp
namespace net
{
class Module : public Cmd
{
public:
    Module();
    virtual ~Module();
    
    // HTTP 请求处理入口
    virtual bool AnyMessage(const tagMsgShell& stMsgShell,
                           const HttpMsg& oInHttpMsg) = 0;
    
    // 从 Cmd 继承 (Module 中不使用)
    virtual bool AnyMessage(const tagMsgShell& stMsgShell,
                           const MsgHead& oInMsgHead,
                           const MsgBody& oInMsgBody) override
    {
        return false;  // 不处理内部协议
    }
    
    // 模块路径
    const std::string& GetModulePath() const;
    void SetModulePath(const std::string& strPath);
    
private:
    std::string m_strModulePath;
};
}
```

### 9.4 使用示例

```cpp
// 定义 HTTP 模块
class ModuleHello : public net::Module
{
public:
    ModuleHello() { SetClassName("ModuleHello"); }
    
    bool Init() override
    {
        // 加载配置
        return net::GetConfig(m_oConf, net::GetConfigPath() + "Hello.json");
    }
    
    bool AnyMessage(const tagMsgShell& stMsgShell,
                   const HttpMsg& oInHttpMsg) override
    {
        std::string strUri = oInHttpMsg.uri();
        std::string strMethod = oInHttpMsg.method();
        
        // 路由分发
        if (strUri == "/hello" && strMethod == "GET")
        {
            return HandleHello(stMsgShell, oInHttpMsg);
        }
        else if (strUri == "/echo" && strMethod == "POST")
        {
            return HandleEcho(stMsgShell, oInHttpMsg);
        }
        else
        {
            return Send404(stMsgShell);
        }
    }
    
private:
    bool HandleHello(const tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
    {
        HttpMsg oOutHttpMsg;
        oOutHttpMsg.set_status_code(200);
        oOutHttpMsg.set_body("Hello, Thunder!");
        
        return GetLabor()->SendTo(stMsgShell, oOutHttpMsg, nullptr);
    }
    
    bool HandleEcho(const tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
    {
        HttpMsg oOutHttpMsg;
        oOutHttpMsg.set_status_code(200);
        oOutHttpMsg.set_body(oInHttpMsg.body());  // 回显
        
        return GetLabor()->SendTo(stMsgShell, oOutHttpMsg, nullptr);
    }
    
    bool Send404(const tagMsgShell& stMsgShell)
    {
        HttpMsg oOutHttpMsg;
        oOutHttpMsg.set_status_code(404);
        oOutHttpMsg.set_body("Not Found");
        
        return GetLabor()->SendTo(stMsgShell, oOutHttpMsg, nullptr);
    }
    
    util::CJsonObject m_oConf;
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

### 9.5 Module 与 Cmd 的区别

| 特性 | Cmd | Module |
|------|-----|--------|
| 协议类型 | 内部二进制协议 | HTTP/HTTPS |
| 请求解析 | Protobuf/MsgBody | HttpMsg |
| 路由方式 | 命令号 (cmd) | URL 路径 |
| 响应方式 | SendToClient | SendTo(HttpMsg) |
| 使用场景 | 内部服务通信 | 外部 HTTP 接入 |

---

## 10. Center节点

### 10.1 功能概述

Center 是**注册发现中心**，支持以下功能：

| 功能 | 描述 |
|------|------|
| 节点注册/注销 | Worker 节点启动时向 Center 注册 |
| 状态上报 | 节点定期向 Center 上报负载信息 |
| 路由同步 | 将在线节点列表分发给各节点 |
| Raft 集群 | 支持多 Center 组成的 Raft 集群 |

### 10.2 目录结构

```
code/Center/
├── src/
│   ├── CmdNodeRegister/        # 节点注册
│   │   ├── CmdNodeRegister.cpp
│   │   └── CmdNodeRegister.hpp
│   ├── CmdNodeReport/         # 状态上报
│   ├── CmdNodeDisconnect/     # 节点断开
│   ├── CmdRaftRequestVote/    # Raft 投票
│   ├── CmdRaftAppendEntries/  # Raft 日志追加
│   ├── ModuleAdmin/           # 管理接口
│   ├── SessionOnlineNodes.cpp # 在线节点会话
│   ├── SessionOnlineNodes.hpp
│   ├── SessionRaftCluster.cpp # Raft 集群会话
│   └── SessionRaftCluster.hpp
└── CMakeLists.txt
```

### 10.3 核心命令

#### 10.3.1 节点注册 (CmdNodeRegister)

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
    // 从数据库加载已有节点
    bool InitFromDb(const util::CJsonObject& oDbConf);
    
    // 从本地配置加载
    bool InitFromLocal(const util::CJsonObject& oLocalConf);
    
private:
    SessionOnlineNodes* m_pSessionOnlineNodes = nullptr;  // 在线节点会话
};
}
```

#### 10.3.2 状态上报 (CmdNodeReport)

节点定期向 Center 上报状态，Center 更新节点信息并同步路由。

### 10.4 Raft 集群

Center 使用 **Raft 共识协议** 实现高可用：

- **Leader 选举**: 通过 RequestVote 消息选举 Leader
- **日志复制**: 通过 AppendEntries 消息同步注册信息
- **故障转移**: Leader 故障后重新选举

#### 10.4.1 会话管理

```cpp
// Raft 集群状态会话
class SessionRaftCluster : public net::Session
{
public:
    SessionRaftCluster();
    
    E_CMD_STATUS Timeout() override;
    
    // Raft 状态
    enum class State { Follower, Candidate, Leader };
    
    State GetState() const { return m_eState; }
    void SetState(State eState);
    
    // 选举计时器
    void ResetElectionTimer();
    void StartElection();
    
    // 日志复制
    bool AppendEntries(const std::string& strEntries);
    std::vector<std::string> GetLog(size_t uiStartIndex);
    
private:
    State m_eState = State::Follower;
    uint64 m_uiCurrentTerm = 0;
    std::string m_strVotedFor;
    std::vector<std::string> m_vecLog;
    size_t m_uiCommitIndex = 0;
    size_t m_uiLastApplied = 0;
};
```

### 10.5 管理接口

Center 提供 HTTP 管理界面：

- 节点列表查看
- 集群状态监控
- 配置管理

---

## 11. 编解码器体系

### 11.1 编解码器架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          外部协议                                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐        │
│  │ HttpCodec  │  │ HttpsCodec │  │ Thunder    │  │ WebSocket  │        │
│  │            │  │            │  │ Codec      │  │ Codec      │        │
│  │ HTTP/1.1   │  │ TLS/SSL   │  │ 二进制协议  │  │ JSON/PB    │        │
│  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘        │
│        │                │                │                │              │
│        └────────────────┴────────────────┴────────────────┘              │
│                                 │                                        │
│                                 ▼                                        │
│                    ┌────────────────────┐                               │
│                    │   CodecFactory     │                               │
│                    │   (编解码工厂)      │                               │
│                    └────────────────────┘                               │
│                                 │                                        │
├─────────────────────────────────┼───────────────────────────────────────┤
│                          内部协议                                         │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                         MsgHead (12字节)                         │   │
│  │  ┌──────────────┬──────────────┬──────────────┐                   │   │
│  │  │    cmd       │     seq      │  msgbody_len │                   │   │
│  │  │   (4字节)    │    (4字节)   │    (4字节)   │                   │   │
│  │  └──────────────┴──────────────┴──────────────┘                   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                         MsgBody                                  │   │
│  │  ┌──────────────┬──────────────┬──────────────┐                   │   │
│  │  │    body       │     req      │     rsp      │                   │   │
│  │  │  (业务数据)   │  (请求标识)  │  (响应标识)  │                   │   │
│  │  └──────────────┴──────────────┴──────────────┘                   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 11.2 编解码器类型

| 编解码器 | 用途 | 协议格式 |
|---------|------|----------|
| **ThunderCodec** | 内部二进制协议 | MsgHead + MsgBody (Protobuf) |
| **HttpCodec** | HTTP/1.1 | RFC 7230 |
| **HttpsCodec** | HTTPS | TLS + HTTP |
| **CodecWebSocketJson** | WebSocket + JSON | RFC 6455 + JSON |
| **CodecWebSocketPb** | WebSocket + Protobuf | RFC 6455 + Protobuf |
| **CodecWebSocketPbApp** | WebSocket + 应用层协议 | 自定义 |
| **AppMsgCodec** | 应用层消息 | 自定义封装 |

### 11.3 核心文件

- **通用定义**: [CodecCommon.hpp](file:///workspace/code/Net/include/codec/CodecCommon.hpp)
- **HTTP 编解码**: [HttpCodec.hpp](file:///workspace/code/Net/include/codec/HttpCodec.hpp)

### 11.4 ThunderCodec 协议格式

```cpp
// 消息头 (固定 12 字节，网络字节序)
struct MsgHead
{
    uint32 cmd;           // 命令号 (4字节)
    uint32 seq;           // 序列号 (4字节)
    uint32 msgbody_len;   // 消息体长度 (4字节)
};

// 消息体
struct MsgBody
{
    string body;          // 业务数据 (通常是 Protobuf 序列化)
    string req;           // 请求标识
    string rsp;          // 响应标识
};

// 编码示例
MsgHead oHead;
oHead.set_cmd(CMD_GET_TOKEN);
oHead.set_seq(GetLabor()->GetSequence());
oHead.set_msgbody_len(oBody.ByteSize());

// 发送
SendTo(stMsgShell, oHead, oBody);
```

### 11.5 CodecFactory 编解码工厂

```cpp
// 根据连接类型创建 Codec
ThunderCodec* CreateCodec(E_CODEC_TYPE eType)
{
    switch (eType)
    {
        case CODEC_HTTP:
            return new HttpCodec();
        case CODEC_HTTPS:
            return new HttpsCodec();
        case CODEC_WEBSOCKET_JSON:
            return new CodecWebSocketJson();
        case CODEC_WEBSOCKET_PB:
            return new CodecWebSocketPb();
        case CODEC_THUNDER:
        case CODEC_PB_INTERNAL:
        default:
            return new ThunderCodec();
    }
}
```

---

## 12. 协程支持

### 12.1 C++20 协程概述

Thunder 框架提供 **C++20 协程** 支持，简化异步编程。

### 12.2 核心文件

- **协程基础**: [Coroutine20.hpp](file:///workspace/code/Net/include/coro/Coroutine20.hpp)
- **协程 Step**: [StepCo20.hpp](file:///workspace/code/Net/include/coro/StepCo20.hpp)
- **Awaitable**: [Awaitable.hpp](file:///workspace/code/Net/include/coro/Awaitable.hpp)

### 12.3 AsyncTask 类型

```cpp
namespace net
{
struct AsyncTask
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
    
    explicit AsyncTask(handle_type h);
    AsyncTask(const AsyncTask&) = delete;
    AsyncTask(AsyncTask&& other) noexcept;
    ~AsyncTask();
    
    struct promise_type
    {
        StepCo20* stepAutoNotify_{nullptr};  // 关联的 Step
        
        template<typename... ExtraArgs>
        promise_type(StepCo20& step, ExtraArgs&&...) noexcept
            : stepAutoNotify_(&step) {}
        
        AsyncTask get_return_object();
        std::suspend_never initial_suspend() noexcept;  // 不挂起
        void return_void() noexcept;
        std::suspend_always final_suspend() noexcept;   // 始终挂起
        
        std::suspend_never yield_value(...) { /* ... */ }
    };
    
    handle_type native_handle() const noexcept;
    
private:
    handle_type coro_;
};
}
```

### 12.4 StepCo20 协程 Step

```cpp
class StepCo20 : public Step
{
public:
    StepCo20();
    StepCo20(const tagMsgShell& stMsgShell);
    StepCo20(const tagMsgShell&, const MsgHead&);
    StepCo20(const tagMsgShell&, const MsgHead&, const MsgBody&);
    
    E_CMD_STATUS Callback(...) override;
    E_CMD_STATUS Timeout() override;
    
protected:
    virtual void OnCoroutineStart() {}      // 协程开始
    virtual void OnCoroutineReturn() {}     // 协程返回
    
    template<typename Awaitable>
    auto await(Awaitable&& awaitable)
    {
        return std::forward<Awaitable>(awaitable);
    }
};
```

### 12.5 使用示例

```cpp
// 定义协程函数
net::AsyncTask HttpRequestCo(StepCo20& step,
                             const std::string& strUrl,
                             const std::string& strBody)
{
    // 1. 发送 HTTP 请求
    co_await HttpRequestAwaitable(step, strUrl, strBody);
    
    // 2. 获取响应
    auto response = step.GetResponse();
    
    // 3. 处理响应
    if (response.code == 200)
    {
        // 继续处理
    }
    
    // 4. 协程结束
    co_return;
}

// 使用协程 Step
class HttpRequestStep : public net::StepCo20
{
public:
    HttpRequestStep(const tagMsgShell& stMsgShell,
                   const MsgHead& oMsgHead,
                   const MsgBody& oMsgBody,
                   const std::string& strUrl)
        : net::StepCo20(stMsgShell, oMsgHead, oMsgBody)
        , m_strUrl(strUrl) {}
    
    E_CMD_STATUS Emit(...) override
    {
        // 启动协程
        HttpRequestCo(*this, m_strUrl, m_strBody);
        return STATUS_CMD_RUNNING;
    }
    
    void OnCoroutineReturn() override
    {
        // 发送响应
        GetLabor()->SendToClient(GetReqMsgShell(), GetReqMsgHead(), m_oResponse);
    }
    
private:
    std::string m_strUrl;
    HttpMsg m_oResponse;
};
```

---

## 13. 存储集成

### 13.1 Redis 操作

#### 13.1.1 核心文件

- [RedisOperator.hpp](file:///workspace/code/Net/include/storage/RedisOperator.hpp)
- [RedisStep.hpp](file:///workspace/code/Net/include/step/RedisStep.hpp)

#### 13.1.2 RedisStep 使用

```cpp
// Redis 异步操作
class GetUserRedisStep : public net::RedisStep
{
public:
    GetUserRedisStep(...)
        : net::RedisStep(stMsgShell, oMsgHead, oMsgBody) {}
    
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                         const MsgHead& oInMsgHead,
                         const MsgBody& oInMsgBody,
                         void* data) override
    {
        redisReply* reply = static_cast<redisReply*>(data);
        
        if (reply && reply->type == REDIS_REPLY_STRING)
        {
            std::string value(reply->str, reply->len);
            // 处理结果
        }
        
        return STATUS_CMD_COMPLETE;
    }
};

// 发送 Redis 命令
GetLabor()->AutoRedisCmd("127.0.0.1", 6379, pRedisStep, "GET", "user:123");
GetLabor()->AutoRedisCmd("127.0.0.1", 6379, pRedisStep, "HGETALL", "user:123");
```

### 13.2 MySQL 操作

#### 13.2.1 核心文件

- [DbOperator.hpp](file:///workspace/code/Net/include/storage/DbOperator.hpp)
- [MysqlStep.hpp](file:///workspace/code/Net/include/step/MysqlStep.hpp)

#### 13.2.2 MysqlStep 使用

```cpp
class QueryUserStep : public net::MysqlStep
{
public:
    QueryUserStep(...)
        : net::MysqlStep(stMsgShell, oMsgHead, oMsgBody) {}
    
    E_CMD_STATUS Callback(const tagMsgShell& stMsgShell,
                         const MsgHead& oInMsgHead,
                         const MsgBody& oInMsgBody,
                         void* data) override
    {
        // 处理查询结果
        MYSQL_RES* pResult = static_cast<MYSQL_RES*>(data);
        
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(pResult)))
        {
            // 读取每行数据
        }
        
        return STATUS_CMD_COMPLETE;
    }
};

// 执行查询
GetLabor()->AutoMysqlCmd(pQueryStep);
```

---

## 14. 依赖关系

### 14.1 项目依赖树

```
Thunder
│
├── Net (网络库)
│   ├── Util (工具库)
│   │   ├── log4cplus (日志)
│   │   ├── hiredis (Redis)
│   │   ├── libcurl (HTTP)
│   │   ├── cJSON (JSON)
│   │   └── OpenSSL (加密)
│   │
│   ├── Proto (协议库)
│   │   ├── protobuf (Protocol Buffers)
│   │   └── absl (Abseil)
│   │
│   └── 3party (第三方)
│       ├── libev (事件循环)
│       ├── hiredis_vip (Redis)
│       └── ...
│
├── Center (可选)
│   └── Raft 集群实现
│
├── Logic (可选)
│   └── 业务逻辑
│
├── Interface (可选)
│   └── 登录接口
│
└── Hello (示例)
    └── Demo 实现
```

### 14.2 外部依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| CMake | >= 3.20 | 构建系统 |
| GCC/Clang | C++20 支持 | 编译器 |
| OpenSSL | 1.1.0+ | TLS/SSL |
| protobuf | 3.x | 协议序列化 |
| libev | 4.x | 事件循环 |
| log4cplus | 最新 | 日志 |
| hiredis | 最新 | Redis 客户端 |
| libcurl | 最新 | HTTP 客户端 |

---

## 15. 编译构建

### 15.1 环境要求

- CMake >= 3.20
- C++20 编译器 (GCC 10+ 或 Clang 12+)
- OpenSSL 开发包

### 15.2 一键构建

```bash
# 1. 初始化子模块
git submodule update --init --recursive

# 2. 配置 CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 3. 构建第三方库
cmake --build build --target thirdparty_deploy -j1

# 4. 构建主工程
cmake --build build -j1

# 5. 安装
cmake --install build
```

### 15.3 CMake 选项

```cmake
# 可选构建项
THUNDER_BUILD_CENTER          # 构建 Center 节点 (默认 ON)
THUNDER_BUILD_HELLO_PLUGINS   # 构建 Hello 插件 (默认 ON)
THUNDER_BUILD_NODE_PLUGINS    # 构建节点插件 (默认 ON)
THUNDER_DEPLOY_AUTO           # 自动部署到 deploy/ (默认 ON)
THUNDER_INCLUDE_3PARTY        # 包含第三方库 (默认 ON)
THUNDER_BUILD_TESTS           # 构建测试 (默认 OFF)
```

---

## 16. 部署运行

### 16.1 目录结构

```
deploy/
├── bin/                       # 可执行文件
├── lib/                       # 库文件
├── plugins/                   # 插件
├── Center/
│   ├── bin/                   # Center 可执行文件
│   ├── conf/                  # 配置文件
│   ├── log/                   # 日志
│   └── scripts/               # 脚本
├── HelloHttp/
├── HelloWs/
├── HelloHttps/
├── Logic/
├── Interface/
└── nodes.sh                   # 启停脚本
```

### 16.2 启停命令

```bash
# 启动所有节点
./nodes.sh start all

# 停止所有节点
./nodes.sh stop all

# 重启所有节点
./nodes.sh restart all

# 重启指定节点
./nodes.sh restart Center
./nodes.sh restart Logic
./nodes.sh restart Interface

# 查看状态
./nodes.sh status
```

### 16.3 配置文件示例

```json
// deploy/Logic/conf/Logic.json
{
    "server_name": "Logic",
    "node_type": "LOGIC",
    "process_num": 4,
    "inner_host": "127.0.0.1",
    "inner_port": 27008,
    "access_host": "127.0.0.1",
    "access_port": 27008,
    "center": [
        {"host": "127.0.0.1", "port": 16068}
    ],
    "cmd": [
        {"cmd": 10001, "so": "plugins/CmdGetToken.so", "entry": "CreateCmd", "version": 1}
    ],
    "module": [
        {"url_path": "/api/*", "so": "plugins/ModuleLogic.so", "entry": "CreateModule"}
    ],
    "redis": {
        "host": "127.0.0.1",
        "port": 6379
    },
    "mysql": {
        "host": "127.0.0.1",
        "port": 3306,
        "database": "thunder",
        "user": "root",
        "password": ""
    },
    "io_timeout": 300,
    "log_level": "INFO"
}
```

---

## 17. 插件开发指南

### 17.1 Cmd 插件开发

#### 17.1.1 定义 Cmd 类

```cpp
// CmdGetToken.hpp
#pragma once
#include "cmd/Cmd.hpp"

class CmdGetToken : public net::Cmd
{
public:
    CmdGetToken();
    virtual ~CmdGetToken() = default;
    
    bool Init() override;
    bool AnyMessage(const net::tagMsgShell& stMsgShell,
                   const MsgHead& oMsgHead,
                   const MsgBody& oMsgBody) override;
    
private:
    bool VerifyUser(const std::string& username, const std::string& password);
    std::string GenerateToken(uint32 user_id);
    
    util::CJsonObject m_oConf;
};

// CmdGetToken.cpp
#include "CmdGetToken.hpp"
#include "GetTokenStep.hpp"

CmdGetToken::CmdGetToken()
{
    SetClassName("CmdGetToken");
}

bool CmdGetToken::Init()
{
    return net::GetConfig(m_oConf, net::GetConfigPath() + "Logic.json");
}

bool CmdGetToken::AnyMessage(const net::tagMsgShell& stMsgShell,
                             const MsgHead& oMsgHead,
                             const MsgBody& oMsgBody)
{
    GetTokenReq oReq;
    if (!oReq.ParseFromString(oMsgBody.body()))
    {
        return false;
    }
    
    // 创建业务 Step
    auto pStep = new GetTokenStep(stMsgShell, oMsgHead, oMsgBody,
                                 oReq.username(), oReq.password());
    return net::Launch(pStep);
}

// 导出函数
extern "C" CmdGetToken* CreateCmd()
{
    return new CmdGetToken();
}

extern "C" void DestroyCmd(CmdGetToken* p)
{
    delete p;
}
```

#### 17.1.2 编译配置

```cmake
# code/Logic/CMakeLists.txt
add_library(CmdGetToken SHARED 
    src/CmdGetToken/CmdGetToken.cpp
)

target_link_libraries(CmdGetToken
    Net
    Proto
    Util
)

install(TARGETS CmdGetToken LIBRARY DESTINATION Logic/plugins)
```

### 17.2 Module 插件开发

#### 17.2.1 定义 Module 类

```cpp
// ModuleInterface.hpp
#pragma once
#include "cmd/Module.hpp"

class ModuleInterface : public net::Module
{
public:
    ModuleInterface();
    virtual ~ModuleInterface() = default;
    
    bool Init() override;
    bool AnyMessage(const net::tagMsgShell& stMsgShell,
                   const HttpMsg& oInHttpMsg) override;
    
private:
    bool HandleGenToken(const net::tagMsgShell& stMsgShell,
                       const HttpMsg& oInHttpMsg);
    bool HandleVerifyToken(const net::tagMsgShell& stMsgShell,
                          const HttpMsg& oInHttpMsg);
    
    util::CJsonObject m_oConf;
};

// ModuleInterface.cpp
#include "ModuleInterface.hpp"

ModuleInterface::ModuleInterface()
{
    SetClassName("ModuleInterface");
}

bool ModuleInterface::Init()
{
    return net::GetConfig(m_oConf, net::GetConfigPath() + "Interface.json");
}

bool ModuleInterface::AnyMessage(const net::tagMsgShell& stMsgShell,
                                const HttpMsg& oInHttpMsg)
{
    std::string strUri = oInHttpMsg.uri();
    
    if (strUri == "/Interface/gentoken")
    {
        return HandleGenToken(stMsgShell, oInHttpMsg);
    }
    else if (strUri == "/Interface/verify")
    {
        return HandleVerifyToken(stMsgShell, oInHttpMsg);
    }
    
    return false;
}

bool ModuleInterface::HandleGenToken(const net::tagMsgShell& stMsgShell,
                                     const HttpMsg& oInHttpMsg)
{
    HttpMsg oOutHttpMsg;
    oOutHttpMsg.set_status_code(200);
    oOutHttpMsg.set_body("{\"code\":0,\"token\":\"abc123\"}");
    
    return GetLabor()->SendTo(stMsgShell, oOutHttpMsg, nullptr);
}

// 导出函数
extern "C" ModuleInterface* CreateModule()
{
    return new ModuleInterface();
}

extern "C" void DestroyModule(ModuleInterface* p)
{
    delete p;
}
```

### 17.3 配置注册

```json
// deploy/Interface/conf/Interface.json
{
    "module": [
        {
            "url_path": "/Interface/*",
            "so": "plugins/ModuleInterface.so",
            "entry": "CreateModule",
            "version": 1
        }
    ]
}
```

### 17.4 热更新

```bash
# 发送 SIGUSR1 信号重新加载配置
kill -SIGUSR1 <pid>

# 或使用 centercli 工具
./centercli.py reload --node Logic --config Logic.json
```

---

## 附录

### A. 常用命令号

| 命令号 | 用途 |
|--------|------|
| 1-1000 | 系统保留 |
| 10001 | 获取 Token |
| 10002 | 验证 Token |
| 20001 | 自定义业务 |

### B. 错误码

| 错误码 | 含义 |
|--------|------|
| ERR_OK (0) | 成功 |
| ERR_PARAM (1) | 参数错误 |
| ERR_TIMEOUT (2) | 超时 |
| ERR_NOMEM (3) | 内存不足 |
| ERR_NET (4) | 网络错误 |
| ERR_DB (5) | 数据库错误 |

### C. 相关文档

- [README.md](file:///workspace/README.md) - 项目说明
- [INSTALL.md](file:///workspace/INSTALL.md) - 构建指南
- [deploy/deploy.md](file:///workspace/deploy/deploy.md) - 部署文档

---

*文档生成时间: 2026-05-13*
*框架版本: Thunder Framework v1.0*
