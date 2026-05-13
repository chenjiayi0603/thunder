# Thunder 框架 HTTPS 编解码器实现与原理分析

## 文档概述

本文档对 Thunder 框架中 HTTPS 编解码器的实现进行深度技术分析。Thunder 是陈嘉怡（cjy）的个人开源项目，一个基于 C++20 的实时异步通信分布式后台系统。本文档聚焦于 HTTPS 编解码器的架构设计、核心实现原理、代码细节分析，以及与框架其他组件的协作机制。

**目标读者**：C++ 后端开发者，了解 HTTP/TLS 基本概念，希望深入理解 Thunder 框架内部实现的工程师。

---

## 第一部分：编解码器继承体系总览

### 1.1 四层继承架构

Thunder 框架的编解码器采用典型的继承体系设计，从基类到具体实现共四层，每层职责边界清晰：

```
┌─────────────────────────────────────────────────────────────────────┐
│                        CStreamCodec                                  │
│                   (编解码器类型枚举定义)                              │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    ThunderCodec                               │   │
│  │           (编解码基类，序列化/压缩/加密)                       │   │
│  │                                                                │   │
│  │  ┌────────────────────────────────────────────────────────┐  │   │
│  │  │                    HttpCodec                             │  │   │
│  │  │              (HTTP明文协议处理)                          │  │   │
│  │  │                                                        │  │   │
│  │  │  ┌──────────────────────────────────────────────────┐  │  │   │
│  │  │  │                    HttpsCodec                     │  │  │   │
│  │  │  │              (HTTPS = HTTP + TLS)                 │  │  │   │
│  │  │  │                                                   │  │  │   │
│  │  │  └──────────────────────────────────────────────────┘  │  │   │
│  │  └────────────────────────────────────────────────────────┘  │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.2 E_CODEC_TYPE 枚举定义

在 `StreamCodec.hpp` 中定义的 `E_CODEC_TYPE` 枚举明确了框架支持的所有编解码器类型：

```cpp
enum E_CODEC_TYPE {
    CODEC_UNKNOW = 0,
    CODEC_PB_INTERNAL = 2,   // 内部Protobuf编解码
    CODEC_HTTP = 3,          // HTTP明文编解码
    CODEC_PRIVATE = 4,       // 私有协议编解码
    CODEC_WEBSOCKET_EX_JS = 5,
    CODEC_WEBSOCKET_EX_PB = 6,
    CODEC_TLV = 7,
    CODEC_TEST = 8,
    CODEC_APP = 9,
    CODEC_WEBSOCKET_EX_PB_APP = 10,
    CODEC_HTTPS = 11,        // HTTPS编解码
};
```

**枚举值设计分析**：
- 值从 2 开始，0 和 1 可能预留给系统保留类型
- HTTPS 单独作为一个类型（11），而不是 HTTP 的扩展，体现了设计者对两种协议本质区别的认知
- 多种 WebSocket 变体（JS/PB/APP）反映了框架对不同业务场景的适配

### 1.3 各层职责边界

| 层级 | 类名 | 核心职责 | 关键能力 |
|------|------|----------|----------|
| 第一层 | CStreamCodec | 类型定义、基础接口声明 | E_CODEC_TYPE 枚举 |
| 第二层 | ThunderCodec | 序列化、压缩、加密 | zlib 压缩、RC5 加密、CBuffer 封装 |
| 第三层 | HttpCodec | HTTP 协议解析 | http_parser 集成、请求/响应组装 |
| 第四层 | HttpsCodec | TLS 加解密封装 | OpenSSL 集成、BIO 缓冲、握手管理 |

**设计哲学**：每层只关注自己的核心职责，下层为上层提供基础能力，上层复用下层能力但不关心下层实现细节。这种设计使得：
- HTTP 协议处理逻辑可以独立于 TLS 运行
- HttpsCodec 可以透明支持 TLS 1.2 和 TLS 1.3
- 未来可以方便地添加 HTTP/2 或 HTTP/3 支持

---

## 第二部分：ThunderCodec 基础能力

### 2.1 E_CODEC_STATUS 状态机设计

ThunderCodec 定义了编解码操作可能返回的所有状态：

```cpp
enum E_CODEC_STATUS {
    CODEC_STATUS_OK = 0,        // 成功
    CODEC_STATUS_PAUSE = 1,     // 暂停（需要更多数据或等待）
    CODEC_STATUS_ERR = 2,       // 错误
    CODEC_STATUS_CONN_CLOSE = 3, // 连接关闭
    CODEC_STATUS_TIMEOUT = 4,   // 超时
};
```

**状态机流转逻辑**：

```
                    ┌──────────────┐
                    │    START     │
                    └──────┬───────┘
                           │
                           ▼
              ┌────────────────────────┐
              │      CODEC_STATUS_OK    │◄───────────────┐
              └───────────┬────────────┘                │
                          │                              │
          ┌───────────────┼───────────────┐              │
          ▼               ▼               ▼              │
    ┌──────────┐   ┌──────────┐   ┌──────────────┐      │
    │   PAUSE  │──►│   ERR    │──►│ CONN_CLOSE   │      │
    └──────────┘   └──────────┘   └──────────────┘      │
          │                                                │
          └────────────────────────────────────────────────┘
                          (需要更多数据)
```

**PAUSE 状态的语义**：这是异步通信框架中的关键状态。当返回 PAUSE 时，表示：
- 读场景：需要等待更多网络数据才能继续解析
- 写场景：SSL 引擎需要更多 IO 操作（如握手未完成）
- IO 调度器应该继续监听该连接，不关闭也不报错

### 2.2 压缩与加密管道

ThunderCodec 实现了完整的压缩-加密管道：

```
┌─────────────────────────────────────────────────────────────────┐
│                        发送方向 (Encode)                         │
│                                                                 │
│  MsgHead + MsgBody                                              │
│        │                                                        │
│        ▼                                                        │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐        │
│  │   序列化     │───►│  zlib压缩   │───►│  RC5加密    │───►►   │
│  └─────────────┘    └─────────────┘    └─────────────┘        │
│                                                          │     │
│                                                   CBuffer     │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                        接收方向 (Decode)                        │
│                                                                 │
│  CBuffer                                                         │
│     │                                                            │
│     ▼                                                            │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐        │
│  │  RC5解密    │───►│  zlib解压   │───►│   反序列化   │───►►   │
│  └─────────────┘    └─────────────┘    └─────────────┘        │
│                                                          │     │
│                                                     MsgHead     │
│                                                     MsgBody     │
└─────────────────────────────────────────────────────────────────┘
```

**设计优势**：
- 压缩后再加密可以提高加密效率（压缩数据比明文更随机）
- RC5 对称加密速度快，适合实时通信场景
- 压缩管道可以独立启用/关闭，适应不同网络环境

### 2.3 CBuffer 与编解码交互

`CBuffer` 是 Thunder 框架的二进制数据容器，封装了读写索引管理和动态扩容：

**核心接口**：
- `Write(const char*, size_t)`：追加数据
- `GetRawReadBuffer()`：获取可读数据的原始指针
- `AdvanceReadIndex(size_t)`：推进读指针
- `ReadableBytes()`：获取可读字节数

**与 OpenSSL 的协作方式**：
- `BIO_write()`：接收 CBuffer 中的数据写入 BIO
- `BIO_read()`：从 BIO 读取数据写入 CBuffer
- 这种设计使得 OpenSSL 的内存 BIO 可以无缝对接框架的缓冲系统

---

## 第三部分：HttpCodec 核心实现

### 3.1 http_parser 集成方式

HttpCodec 基于 `http_parser` 库（Node.js 核心团队维护的 C 库）实现 HTTP 解析：

```cpp
// http_parser 回调函数（静态）
static int on_message_begin(http_parser* parser) { ... }
static int on_headers_complete(http_parser* parser) { ... }
static int on_body(http_parser* parser, const char* at, size_t length) { ... }
static int on_message_complete(http_parser* parser) { ... }
```

**解析流程**：

```
CBuffer (原始字节流)
       │
       ▼
┌──────────────────┐
│ http_parser_init │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐     ┌──────────────────┐
│ http_parser_execute│────►│  回调函数填充    │
└──────────────────┘     │   HttpMsg 结构   │
                         └──────────────────┘
                                 │
                                 ▼
                          ┌──────────────┐
                          │   HttpMsg    │
                          │  (解析结果)   │
                          └──────────────┘
```

### 3.2 HTTP 请求解析流程

**HttpMsg 结构体** 包含：
- `method`：GET/POST/PUT/DELETE 等
- `url`：请求路径
- `headers`：键值对 map
- `body`：请求体

**解析过程**：
1. 调用 `http_parser_parse_url()` 解析 URL
2. 逐行解析 HTTP 头
3. 根据 `Content-Length` 或 `Transfer-Encoding` 判断 body 长度
4. body 数据通过 `on_body` 回调累加到 `HttpMsg.body`

### 3.3 HTTP 响应组装

响应组装是编码过程的逆操作：

```cpp
E_CODEC_STATUS HttpCodec::Encode(const HttpMsg& oMsg, util::CBuffer* pBuff)
{
    // 1. 状态行：HTTP/1.1 200 OK
    pBuff->Write("HTTP/1.1 ");
    pBuff->Write(std::to_string(oMsg.iStatusCode));
    pBuff->Write(" ");
    pBuff->Write(getStatusText(oMsg.iStatusCode));
    pBuff->Write("\r\n");
    
    // 2. 响应头
    for (auto& [k, v] : oMsg.headers) {
        pBuff->Write(k);
        pBuff->Write(": ");
        pBuff->Write(v);
        pBuff->Write("\r\n");
    }
    
    // 3. 空行 + body
    pBuff->Write("\r\n");
    pBuff->Write(oMsg.body);
}
```

### 3.4 Chunked Transfer Encoding 支持

对于流式响应，HttpCodec 支持 `Transfer-Encoding: chunked`：

```
┌─────────────────────────────────────────────────────────────┐
│                      Chunked Body 格式                       │
│                                                             │
│  长度(hex)\r\n                                               │
│  数据\r\n                                                    │
│  长度(hex)\r\n                                               │
│  数据\r\n                                                    │
│  ...                                                        │
│  0\r\n\r\n                                                  │
└─────────────────────────────────────────────────────────────┘
```

---

## 第四部分：HttpsCodec 架构设计

### 4.1 继承 HttpCodec 的设计意图

HttpsCodec 继承 HttpCodec 是经过深思熟虑的设计决策：

**核心思想**：HTTPS = HTTP + TLS

这意味着：
- TLS 层负责加解密
- HTTP 层负责协议解析
- 两者是正交的功能，可以组合

**复用优势**：

```
┌─────────────────────────────────────────────────────────────┐
│                    HttpsCodec 设计                           │
│                                                             │
│   ┌─────────────────────────────────────────────────────┐   │
│   │                    HttpsCodec                        │   │
│   │                                                      │   │
│   │   TLS 层：                                          │   │
│   │   ┌─────────────────────────────────────────────┐   │   │
│   │   │  OpenSSL (SSL_CTX, SSL, BIO)               │   │   │
│   │   │  握手管理 / 加解密 / 证书验证                 │   │   │
│   │   └─────────────────────────────────────────────┘   │   │
│   │                        │                             │   │
│   │   HTTP 层：◄───────────┘                             │   │
│   │   ┌─────────────────────────────────────────────┐   │   │
│   │   │              HttpCodec::Decode              │   │   │
│   │   │   解析 HTTP 消息结构                          │   │   │
│   │   └─────────────────────────────────────────────┘   │   │
│   └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

**代码层面的体现**：

```cpp
// HttpsCodec::Decode 中调用基类
E_CODEC_STATUS HttpsCodec::Decode(tagConnectionAttr* pConn, 
                                   MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    // ... TLS 解密到 oPlainRecvBuff ...
    
    // 复用 HttpCodec 解析 HTTP
    return HttpCodec::Decode(&pState->oPlainRecvBuff, oMsgHead, oMsgBody);
}
```

### 4.2 TlsConnState 连接级状态管理

每个连接（fd）维护一个独立的 `TlsConnState` 结构：

```cpp
struct TlsConnState {
    SSL_CTX* pCtx = nullptr;              // SSL 上下文（可共享）
    SSL* pSsl = nullptr;                  // SSL 连接对象（每连接独立）
    bool bServerSide = false;             // 角色标识
    bool bHandshakeDone = false;          // 握手完成标志
    
    util::CBuffer oPlainRecvBuff;          // 解密后的明文缓冲
    util::CBuffer oPendingPlainSend;       // 握手期间的待发送明文
};
```

**状态生命周期**：

```
┌─────────────────────────────────────────────────────────────────┐
│                    TlsConnState 生命周期                         │
│                                                                 │
│  1. 创建 (EnsureState)                                          │
│     ┌──────────────────────────────────────────────┐           │
│     │  SSL_CTX_new() → SSL_new() → BIO_new()       │           │
│     │  设置证书/密钥/角色                            │           │
│     └──────────────────────────────────────────────┘           │
│                         │                                       │
│                         ▼                                       │
│  2. 运行 (Decode/Encode 循环)                                    │
│     ┌──────────────────────────────────────────────┐           │
│     │  FeedCipherToSsl → DoHandshake →             │           │
│     │  DrainSslToPlain → HttpCodec::Decode          │           │
│     └──────────────────────────────────────────────┘           │
│                         │                                       │
│                         ▼                                       │
│  3. 销毁 (RemoveConnection)                                      │
│     ┌──────────────────────────────────────────────┐           │
│     │  SSL_shutdown → SSL_free → SSL_CTX_free      │           │
│     └──────────────────────────────────────────────┘           │
└─────────────────────────────────────────────────────────────────┘
```

### 4.3 BIO 内存型缓冲机制

OpenSSL 的 BIO（Basic Input/Output）是抽象的 IO 接口，`BIO_s_mem()` 创建内存型 BIO：

```
┌─────────────────────────────────────────────────────────────────┐
│                    BIO 内存缓冲机制                              │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                      SSL 对象                            │   │
│  │                                                          │   │
│  │   ┌─────────────────┐         ┌─────────────────┐       │   │
│  │   │   RBIO (读)     │◄────────│    网络数据      │       │   │
│  │   │  (接收密文)     │         │   pConn->pRecvBuff    │   │
│  │   └────────┬────────┘         └─────────────────┘       │   │
│  │            │                                              │   │
│  │            ▼                                              │   │
│  │   ┌─────────────────┐                                     │   │
│  │   │   TLS 引擎       │                                     │   │
│  │   │  (解密处理)      │                                     │   │
│  │   └────────┬────────┘                                     │   │
│  │            │                                              │   │
│  │            ▼                                              │   │
│  │   ┌─────────────────┐         ┌─────────────────┐       │   │
│  │   │   WBIO (写)     │────────►│   密文输出       │       │   │
│  │   │  (发送密文)      │         │   pConn->pSendBuff    │   │
│  │   └─────────────────┘         └─────────────────┘       │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

**为什么使用内存 BIO**：
- 框架已有成熟的网络 IO 管理（Epoll/IO_uring）
- 内存 BIO 让 OpenSSL 可以与现有 IO 系统解耦
- 密文数据的读写完全由框架控制

### 4.4 SSL/SSL_CTX 生命周期管理

**SSL_CTX（SSL Context）**：
- 可在多个 SSL 连接间共享
- 配置证书、密钥、验证策略等
- 在 `EnsureState` 中根据角色（server/client）选择方法：
  - 服务端：`TLS_server_method()`
  - 客户端：`TLS_client_method()`

**SSL（SSL Connection）**：
- 每个连接独立
- 包含当前连接的状态、会话信息等
- 绑定到两个 BIO（RBIO 和 WBIO）

### 4.5 连接角色与证书配置

```cpp
struct HttpsConfig {
    std::string strServerCertFile;     // 服务端证书
    std::string strServerKeyFile;      // 服务端私钥
    std::string strServerCaFile;       // 服务端 CA 证书（验证客户端）
    bool bServerVerifyClient = false;  // 是否验证客户端证书
    std::string strClientCaFile;       // 客户端 CA 证书（验证服务端）
    bool bClientVerifyPeer = false;    // 是否验证服务端证书
};
```

**角色判断逻辑**：

```cpp
auto roleIt = m_mapConnRole.find(iFd);
pState->bServerSide = (roleIt != m_mapConnRole.end()) ? roleIt->second : true;
```

默认行为：未显式设置角色的连接被视为服务端，这是一个安全假设（大多数 HTTPS 服务是服务端）。

---

## 第五部分：HTTPS 读路径全链路分析

### 5.1 读路径数据流总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        HTTPS 读路径全链路                                 │
│                                                                         │
│  [Client]  ──TLS密文──►  pConn->pRecvBuff                               │
│                              │                                           │
│                              ▼                                           │
│                     FeedCipherToSsl                                     │
│                              │                                           │
│                              ▼                                           │
│                      RBIO (SSL读缓冲)                                    │
│                              │                                           │
│                              ▼                                           │
│                     DoHandshake                                         │
│                   (处理 TLS 握手)                                        │
│                              │                                           │
│                              ▼                                           │
│                      SSL_read                                            │
│                   (解密出明文)                                            │
│                              │                                           │
│                              ▼                                           │
│                 oPlainRecvBuff                                          │
│               (解密后的明文缓冲)                                          │
│                              │                                           │
│                              ▼                                           │
│               HttpCodec::Decode                                         │
│              (解析 HTTP 消息结构)                                         │
│                              │                                           │
│                              ▼                                           │
│                   MsgHead + MsgBody                                      │
│               (框架内部消息格式)                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 5.2 FeedCipherToSsl：密文注入

```cpp
E_CODEC_STATUS HttpsCodec::FeedCipherToSsl(tagConnectionAttr* pConn, 
                                            TlsConnState* pState)
{
    int iLen = pConn->pRecvBuff->ReadableBytes();
    if (iLen <= 0) return CODEC_STATUS_OK;
    
    // 将网络缓冲中的密文写入 SSL 的读 BIO
    int n = BIO_write(
        SSL_get_rbio(pState->pSsl), 
        pConn->pRecvBuff->GetRawReadBuffer(), 
        iLen
    );
    
    // 消费已写入 BIO 的数据
    pConn->pRecvBuff->AdvanceReadIndex(n);
    
    return CODEC_STATUS_OK;
}
```

**关键点**：
- `SSL_get_rbio()` 获取 SSL 绑定的读 BIO
- `BIO_write()` 返回实际写入的字节数
- 必须调用 `AdvanceReadIndex()` 标记数据已消费

### 5.3 DoHandshake：握手状态机驱动

```cpp
E_CODEC_STATUS HttpsCodec::DoHandshake(tagConnectionAttr* pConn, 
                                        TlsConnState* pState, 
                                        util::CBuffer* pOutBuff)
{
    // 握手已完成，直接返回
    if (pState->bHandshakeDone) return CODEC_STATUS_OK;
    
    // 执行握手（可能是多轮）
    int ret = SSL_do_handshake(pState->pSsl);
    
    if (ret == 1) {
        // 握手成功
        pState->bHandshakeDone = true;
        DrainOutboundCipher(pState, pOutBuff);
        return CODEC_STATUS_OK;
    }
    
    int err = SSL_get_error(pState->pSsl, ret);
    
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // 非阻塞 IO，需要更多 IO 操作
        DrainOutboundCipher(pState, pOutBuff);
        return CODEC_STATUS_PAUSE;  // 关键：暂停等待
    }
    
    return CODEC_STATUS_ERR;
}
```

**握手状态机解释**：

```
┌─────────────────────────────────────────────────────────────────┐
│                    TLS 握手状态机                                │
│                                                                 │
│  ┌─────────┐                                                    │
│  │  IDLE   │                                                    │
│  └───┬─────┘                                                    │
│      │ 调用 SSL_do_handshake                                    │
│      ▼                                                          │
│  ┌─────────────────────────────────────────┐                   │
│  │          握手进行中                       │                   │
│  │  ┌─────────┐  ┌─────────┐              │                   │
│  │  │ WANT_READ│  │ WANT_WRITE│              │                   │
│  │  └────┬────┘  └────┬────┘              │                   │
│  │       │            │                    │                   │
│  │       ▼            ▼                    │                   │
│  │  ┌─────────┐  ┌─────────┐              │                   │
│  │  │ 需要接收 │  │ 需要发送 │              │                   │
│  │  │ 更多数据 │  │ 更多数据 │              │                   │
│  │  └────┬────┘  └────┬────┘              │                   │
│  └───────┼────────────┼───────────────────┘                   │
│          │            │                                        │
│          └─────┬──────┘                                        │
│                │                                                │
│                ▼                                                │
│          ┌─────────┐                                            │
│          │  DONE   │  ← bHandshakeDone = true                   │
│          └─────────┘                                            │
└─────────────────────────────────────────────────────────────────┘
```

### 5.4 DrainSslToPlain：明文消费

```cpp
E_CODEC_STATUS HttpsCodec::DrainSslToPlain(TlsConnState* pState)
{
    if (!pState->bHandshakeDone) return CODEC_STATUS_OK;
    
    char buf[16384];
    while (true) {
        int n = SSL_read(pState->pSsl, buf, sizeof(buf));
        
        if (n > 0) {
            // 成功解密，追加到明文缓冲
            pState->oPlainRecvBuff.Write(buf, n);
        } else {
            int err = SSL_get_error(pState->pSsl, n);
            
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                break;  // 非阻塞，继续等待
            }
            
            if (err == SSL_ERROR_ZERO_RETURN) {
                // 对端关闭连接（优雅关闭）
                break;
            }
            
            break;  // 其他错误
        }
    }
    return CODEC_STATUS_OK;
}
```

### 5.5 WANT_READ/WANT_WRITE 处理

这两个错误码是 OpenSSL 非阻塞 IO 的核心机制：

| 错误码 | 含义 | 处理方式 |
|--------|------|----------|
| `SSL_ERROR_WANT_READ` | SSL 层需要更多输入数据 | 继续等待可读事件 |
| `SSL_ERROR_WANT_WRITE` | SSL 层需要完成写入 | 继续等待可写事件 |
| `SSL_ERROR_ZERO_RETURN` | 对端发送了 close_notify | 准备关闭连接 |

**为什么返回 PAUSE**：

```cpp
if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    DrainOutboundCipher(pState, pOutBuff);
    return CODEC_STATUS_PAUSE;
}
```

返回 `CODEC_STATUS_PAUSE` 告诉 IO 调度器：
- 当前连接需要继续监听
- 不是错误，只是需要更多 IO 操作
- 下一轮 IO 事件触发时继续处理

---

## 第六部分：HTTPS 写路径全链路分析

### 6.1 写路径数据流总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        HTTPS 写路径全链路                                 │
│                                                                         │
│   MsgHead + MsgBody                                                     │
│        │                                                                │
│        ▼                                                                │
│   HttpCodec::Encode                                                     │
│   (组装 HTTP 明文)                                                       │
│        │                                                                │
│        ▼                                                                │
│   oPlainBuff (HTTP 明文缓冲)                                             │
│        │                                                                │
│        ▼                                                                │
│   EncryptPlain                                                          │
│   ┌───────────────────────────────────────────────────────────────┐   │
│   │ 1. 握手未完成 → oPendingPlainSend 暂存                          │   │
│   │ 2. 握手完成 → SSL_write 加密                                    │   │
│   └───────────────────────────────────────────────────────────────┘   │
│        │                                                                │
│        ▼                                                                │
│   DrainOutboundCipher                                                   │
│   (从 WBIO 取出密文)                                                     │
│        │                                                                │
│        ▼                                                                │
│   pConn->pSendBuff                                                       │
│   (等待网络发送)                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 6.2 EncodeToConnection：两阶段设计

```cpp
E_CODEC_STATUS HttpsCodec::EncodeToConnection(tagConnectionAttr* pConn,
                                                const MsgHead& oMsgHead, 
                                                const MsgBody& oMsgBody,
                                                util::CBuffer* pBuff)
{
    // 阶段1：生成 HTTP 明文
    util::CBuffer oPlainBuff;
    E_CODEC_STATUS eStatus = HttpCodec::Encode(oMsgHead, oMsgBody, &oPlainBuff);
    if (eStatus != CODEC_STATUS_OK) return eStatus;
    
    // 阶段2：通过 SSL_write 加密
    return EncryptPlain(pConn, EnsureState(pConn),
                        oPlainBuff.GetRawReadBuffer(),
                        oPlainBuff.ReadableBytes(), pBuff);
}
```

**两阶段的设计价值**：
1. 明文生成与加密解耦
2. 可以独立测试 HTTP 协议部分
3. 便于调试（可以打印中间明文）

### 6.3 EncryptPlain：加密与暂存

```cpp
E_CODEC_STATUS HttpsCodec::EncryptPlain(tagConnectionAttr* pConn,
                                          TlsConnState* pState,
                                          const char* pData, 
                                          size_t iLen, 
                                          util::CBuffer* pBuff)
{
    // 握手未完成，暂存明文
    if (!pState->bHandshakeDone) {
        pState->oPendingPlainSend.Write(pData, iLen);
        return CODEC_STATUS_PAUSE;
    }
    
    // 执行加密
    int n = SSL_write(pState->pSsl, pData, iLen);
    
    if (n <= 0) {
        int err = SSL_get_error(pState->pSsl, n);
        
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // 部分写入，暂存剩余数据
            pState->oPendingPlainSend.Write(pData + n, iLen - n);
            DrainOutboundCipher(pState, pBuff);
            return CODEC_STATUS_PAUSE;
        }
        
        return CODEC_STATUS_ERR;
    }
    
    // 成功写入，刷新密文到输出缓冲
    DrainOutboundCipher(pState, pBuff);
    return CODEC_STATUS_OK;
}
```

### 6.4 握手期间明文暂存机制

**oPendingPlainSend 的作用**：

```
┌─────────────────────────────────────────────────────────────────┐
│              握手期间明文暂存机制                                 │
│                                                                 │
│  时间线：                                                        │
│                                                                 │
│  T1: 应用层调用 EncodeToConnection                               │
│      │                                                          │
│      ▼                                                          │
│  T2: 检查 bHandshakeDone = false                                │
│      │                                                          │
│      ▼                                                          │
│  T3: 明文写入 oPendingPlainSend                                  │
│      │                                                          │
│      │    ┌──────────────────────────────────┐                 │
│      │    │  oPendingPlainSend               │                 │
│      │    │  [HTTP GET /api/data HTTP/1.1]  │                 │
│      │    │  [Host: example.com]             │                 │
│      │    └──────────────────────────────────┘                 │
│      ▼                                                          │
│  T4: 返回 CODEC_STATUS_PAUSE                                     │
│                                                                 │
│  ... 等待握手完成 ...                                            │
│                                                                 │
│  T5: 握手完成，Decode 中检测到 bHandshakeDone                    │
│      │                                                          │
│      ▼                                                          │
│  T6: 将 oPendingPlainSend 中数据写入 SSL_write                  │
│      │                                                          │
│      ▼                                                          │
│  T7: 清空 oPendingPlainSend                                      │
└─────────────────────────────────────────────────────────────────┘
```

### 6.5 DrainOutboundCipher：密文刷新

```cpp
void HttpsCodec::DrainOutboundCipher(TlsConnState* pState, util::CBuffer* pOutBuff)
{
    BIO* wbio = SSL_get_wbio(pState->pSsl);
    
    if (!wbio || BIO_pending(wbio) <= 0) return;
    
    char buf[16384];
    while (true) {
        int n = BIO_read(wbio, buf, sizeof(buf));
        if (n <= 0) break;
        pOutBuff->Write(buf, n);
    }
}
```

**BIO_pending 的作用**：
- 检查写 BIO 中是否有待读取的数据
- 避免不必要的空循环
- 提高效率

---

## 第七部分：TLS 握手状态机详解

### 7.1 握手驱动模型

Thunder 采用**应用层驱动**的握手模型：

```cpp
// 每次 Decode 调用时尝试推进握手
E_CODEC_STATUS eHandshakeStatus = DoHandshake(pConn, pState, pConn->pSendBuff.get());
if (eHandshakeStatus == CODEC_STATUS_PAUSE) return CODEC_STATUS_PAUSE;
```

**优势**：
- 与框架的协程模型天然契合
- 可以在 IO 等待时让出协程
- 简化状态管理

### 7.2 TLS 1.2 vs TLS 1.3 透明处理

框架对 TLS 版本透明处理：

```cpp
// 根据方法自动选择支持的最高版本
pState->pCtx = SSL_CTX_new(
    pState->bServerSide ? TLS_server_method() : TLS_client_method()
);
```

- `TLS_server_method()` / `TLS_client_method()` 自动启用支持的最高版本
- TLS 1.3 的 1-RTT 握手和 TLS 1.2 的完整握手都被支持
- 应用代码无需关心版本差异

### 7.3 多轮 WANT_READ/WANT_WRITE

典型 TLS 握手流程：

```
┌─────────────────────────────────────────────────────────────────┐
│                    TLS 握手流程（简化）                           │
│                                                                 │
│  Client                              Server                    │
│    │                                    │                       │
│    │────────── ClientHello ────────────►│                       │
│    │   (可能触发 WANT_WRITE)            │                       │
│    │                                    │                       │
│    │◄────────── ServerHello ───────────│                       │
│    │   (触发 WANT_READ)                 │                       │
│    │                                    │                       │
│    │◄────── Certificate + CertVerify ───│                       │
│    │   (触发 WANT_READ)                 │                       │
│    │                                    │                       │
│    │◄──────── Finished ─────────────────│                       │
│    │   (触发 WANT_READ)                 │                       │
│    │                                    │                       │
│    │────────── Finished ──────────────►│                       │
│    │   (可能触发 WANT_WRITE)            │                       │
│    │                                    │                       │
│    │◄══════════ 数据传输 ════════════════│                       │
└─────────────────────────────────────────────────────────────────┘
```

每次 IO 操作可能触发 WANT_READ 或 WANT_WRITE，框架通过返回 PAUSE 并继续监听来处理。

### 7.4 握手完成后的处理

```cpp
if (pState->bHandshakeDone) {
    // 1. 消费暂存的待发送明文
    if (pState->oPendingPlainSend.ReadableBytes() > 0) {
        EncryptPlain(pConn, pState, 
                     pState->oPendingPlainSend.GetRawReadBuffer(),
                     pState->oPendingPlainSend.ReadableBytes(),
                     pConn->pSendBuff.get());
        pState->oPendingPlainSend.Clear();
    }
    
    // 2. 解密应用数据
    DrainSslToPlain(pState);
    
    // 3. 解析 HTTP
    return HttpCodec::Decode(&pState->oPlainRecvBuff, oMsgHead, oMsgBody);
}
```

---

## 第八部分：连接生命周期与安全防御

### 8.1 连接建立流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    HTTPS 连接建立流程                            │
│                                                                 │
│  1. TCP 连接建立 (accept/connect)                                │
│         │                                                       │
│         ▼                                                       │
│  2. 设置连接角色                                                 │
│     ┌────────────────────────────────────────────┐             │
│     │ SetConnectionRole(fd, true/false)          │             │
│     │  - true  = 服务端 (接收连接)                │             │
│     │  - false = 客户端 (发起连接)                │             │
│     └────────────────────────────────────────────┘             │
│         │                                                       │
│         ▼                                                       │
│  3. 首次 Decode 时创建 TlsConnState                            │
│     ┌────────────────────────────────────────────┐             │
│     │ EnsureState(pConn)                          │             │
│     │  - 创建 SSL_CTX                            │             │
│     │  - 创建 SSL                                │             │
│     │  - 创建 RBIO/WBIO                          │             │
│     │  - 设置 SSL 状态 (accept/connect)          │             │
│     └────────────────────────────────────────────┘             │
│         │                                                       │
│         ▼                                                       │
│  4. TLS 握手 (DoHandshake)                                      │
└─────────────────────────────────────────────────────────────────┘
```

### 8.2 角色注入机制

```cpp
// Worker 中的调用
void Worker::OnConnectionAccepted(int iFd) {
    // 入站连接 = 服务端
    m_pCodec->SetConnectionRole(iFd, true);
}

void Worker::OnConnectionOpened(int iFd) {
    // 出站连接 = 客户端
    m_pCodec->SetConnectionRole(iFd, false);
}
```

**设计原因**：
- SSL_CTX 需要在创建 SSL 之前确定角色
- 但 SSL 在首次 Decode 时才创建（延迟创建）
- 通过 `m_mapConnRole` 缓存角色信息

### 8.3 连接关闭与资源清理

```cpp
void HttpsCodec::RemoveConnection(int iFd)
{
    // 1. 查找 TlsConnState
    auto it = m_mapTlsState.find(iFd);
    if (it == m_mapTlsState.end()) return;
    
    // 2. 销毁 TLS 状态
    DestroyState(*it->second);
    
    // 3. 从映射中移除
    m_mapTlsState.erase(it);
    
    // 4. 清理角色记录
    m_mapConnRole.erase(iFd);
}

void HttpsCodec::DestroyState(TlsConnState& pState)
{
    if (pState.pSsl) {
        SSL_shutdown(pState.pSsl);  // 发送 close_notify
        SSL_free(pState.pSsl);
    }
    if (pState.pCtx) {
        SSL_CTX_free(pState.pCtx);
    }
}
```

### 8.4 Stale Shell 防御机制

Stale Shell（过期响应）问题：连接关闭后，之前的请求的回包可能仍然在 IO 队列中，导致发送到已关闭的 fd。

**Thunder 的防御策略**：

```cpp
// Worker 中的处理
if (connection closed) {
    // 短路：不再发送该连接的待发送数据
    pConn->pSendBuff->Clear();
}
```

### 8.5 StepCo20 协程安全修改

框架修改了协程的行为以避免写入已关闭的连接：

```cpp
// 修改前：协程完成后继续发包
StepCo20::OnComplete() {
    // ... 继续处理 ...
    SendResponse(pConn);  // 可能写到已关闭的 fd
}

// 修改后：协程完成后不再继续发包
StepCo20::OnComplete() {
    // 检查连接状态
    if (pConn->IsClosed()) return;  // 安全退出
    // ...
}
```

---

## 第九部分：与 IoBackend 的协作

### 9.1 多后端架构

Thunder 支持多种 IO 后端：

| 后端 | 适用场景 | 特点 |
|------|----------|------|
| EvIoBackend | Linux 生产环境 | Epoll，高效 |
| UringIoBackend | 高 IOPS 场景 | io_uring，零拷贝 |
| AsioUringBackend | 混合场景 | Asio + io_uring |

### 9.2 TLS 与 IO 后端的协作

**关键约束**：TLS 写操作需要同步执行

```
┌─────────────────────────────────────────────────────────────────┐
│                    TLS 写操作的特殊性                            │
│                                                                 │
│  SSL_write() 返回 WANT_WRITE 时：                               │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  可能原因：                                               │   │
│  │  1. WBIO 缓冲区满，需要消费 (BIO_read from WBIO)         │   │
│  │  2. 内部状态机需要更多输入                                │   │
│  │                                                          │   │
│  │  但 Thunder 的 DrainOutboundCipher 已经处理了情况 1       │   │
│  │  所以情况 2 出现时，返回 PAUSE 让 IO 后端继续监听        │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 9.3 UringIoBackend 的同步写策略

UringIoBackend 的写操作采用同步策略：

```cpp
// 伪代码
ssize_t UringIoBackend::Write(int fd, const void* buf, size_t len) {
    // 对于 TLS 加密后的数据，必须同步写入
    return ::write(fd, buf, len);  // 同步系统调用
}
```

**原因**：
- TLS 写操作可能需要多轮 SSL_write
- 异步提交可能导致状态不一致
- 同步写确保每次写入后可以立即检查结果

### 9.4 事件驱动模型

```
┌─────────────────────────────────────────────────────────────────┐
│                    IO 事件与 TLS 处理的协作                       │
│                                                                 │
│  Epoll/io_uring 事件                                            │
│         │                                                       │
│         ▼                                                       │
│  ┌─────────────────┐                                           │
│  │   可读事件       │                                           │
│  └────────┬────────┘                                           │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────────────────────────────┐                   │
│  │ Decode(pConn, ...)                       │                   │
│  │  ├─ FeedCipherToSsl                      │                   │
│  │  ├─ DoHandshake                         │                   │
│  │  │    └─ PAUSE → 继续监听                │                   │
│  │  ├─ DrainSslToPlain                      │                   │
│  │  └─ HttpCodec::Decode                    │                   │
│  └─────────────────────────────────────────┘                   │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐                                           │
│  │   可写事件       │                                           │
│  └────────┬────────┘                                           │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────────────────────────────┐                   │
│  │ DrainOutboundCipher → Send              │                   │
│  │  消费 WBIO 中的密文，发送到网络           │                   │
│  └─────────────────────────────────────────┘                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 第十部分：配置、部署与测试

### 10.1 HttpsConfig 配置项详解

```cpp
struct HttpsConfig {
    // 服务端配置
    std::string strServerCertFile;     // PEM 格式的服务器证书
    std::string strServerKeyFile;      // PEM 格式的服务器私钥
    std::string strServerCaFile;       // CA 证书（验证客户端用）
    bool bServerVerifyClient = false;  // 是否要求客户端证书
    
    // 客户端配置
    std::string strClientCaFile;       // CA 证书（验证服务器用）
    bool bClientVerifyPeer = false;    // 是否验证服务器证书
};
```

**配置组合**：

| 场景 | 服务端配置 | 客户端配置 |
|------|-----------|-----------|
| 单向认证（仅服务器证书） | cert + key | 可选 CA 验证 |
| 双向认证（双方证书） | cert + key + CA + verify | CA + verify |

### 10.2 证书生成

**自签名测试证书生成**：

```bash
#!/bin/bash
# 生成 CA
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 365 -key ca.key -out ca.crt \
    -subj "/C=CN/ST=Beijing/L=Beijing/O=Thunder/OU=Dev/CN=Thunder CA"

# 生成服务器证书
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr \
    -subj "/C=CN/ST=Beijing/L=Beijing/O=Thunder/OU=Server/CN=localhost"
echo "subjectAltName=DNS:localhost,IP:127.0.0.1" > extfile.cnf
openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key \
    -extfile extfile.cnf -CAcreateserial -out server.crt

# 生成客户端证书（双向认证用）
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr \
    -subj "/C=CN/ST=Beijing/O=Client/CN=Client"
openssl x509 -req -days 365 -in client.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out client.crt
```

### 10.3 框架配置

```cpp
// 在框架初始化时配置
HttpsCodec::HttpsConfig config;
config.strServerCertFile = "/path/to/server.crt";
config.strServerKeyFile = "/path/to/server.key";
config.strServerCaFile = "/path/to/ca.crt";  // 双向认证时配置
config.bServerVerifyClient = true;            // 双向认证时启用

m_pHttpsCodec->SetHttpsConfig(config);
```

### 10.4 集成测试

**使用 curl 测试**：

```bash
# 基础测试
curl -k https://localhost:8443/api/health

# 指定客户端证书（双向认证）
curl --cert client.crt --key client.key --cacert ca.crt \
    https://localhost:8443/api/secure

# 完整验证
curl -v --cert client.crt --key client.key --cacert ca.crt \
    https://localhost:8443/api/health 2>&1 | grep -E "(TLS|SSL|Certificate)"
```

### 10.5 常见问题与排障

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| 握手超时 | 网络延迟/防火墙 | 增加超时配置，检查网络 |
| 证书验证失败 | 证书过期/不受信 | 更新证书，添加 CA |
| 双向认证失败 | 客户端证书问题 | 检查 client.crt 是否由 server CA 签发 |
| WANT_READ/WANT_WRITE 过多 | IO 事件处理不及时 | 检查 IO 线程负载 |
| 连接复用失败 | Session ticket 问题 | 检查 SSL_SESSION 缓存 |

---

## 附录：关键代码索引

### A. 文件位置

| 文件 | 路径 | 说明 |
|------|------|------|
| StreamCodec.hpp | code/Util/src/util/ | 编解码器类型定义 |
| ThunderCodec.hpp | code/Net/src/codec/ | 基类定义 |
| HttpCodec.hpp/cpp | code/Net/src/codec/ | HTTP 明文编解码 |
| HttpsCodec.hpp/cpp | code/Net/src/codec/ | HTTPS 实现 |

### B. 关键函数

| 函数 | 文件 | 作用 |
|------|------|------|
| `InitLibrary()` | HttpsCodec.cpp | OpenSSL 全局初始化 |
| `EnsureState()` | HttpsCodec.cpp | 创建/获取 TlsConnState |
| `FeedCipherToSsl()` | HttpsCodec.cpp | 密文注入 SSL |
| `DoHandshake()` | HttpsCodec.cpp | 握手状态机驱动 |
| `DrainSslToPlain()` | HttpsCodec.cpp | 解密明文消费 |
| `EncryptPlain()` | HttpsCodec.cpp | 明文加密 |
| `DrainOutboundCipher()` | HttpsCodec.cpp | 密文刷新到输出 |
| `RemoveConnection()` | HttpsCodec.cpp | 连接清理 |

### C. 数据结构

| 结构体 | 定义位置 | 用途 |
|--------|----------|------|
| `E_CODEC_TYPE` | StreamCodec.hpp | 编解码器类型枚举 |
| `E_CODEC_STATUS` | ThunderCodec.hpp | 操作状态枚举 |
| `HttpsConfig` | HttpsCodec.hpp | HTTPS 配置 |
| `TlsConnState` | HttpsCodec.hpp | TLS 连接状态 |

---

## 总结

Thunder 框架的 HTTPS 编解码器实现展现了以下设计特点：

1. **清晰的层次划分**：继承体系使得各层职责明确，便于维护和扩展
2. **灵活的角色管理**：服务端/客户端角色通过映射表管理，适应不同场景
3. **高效的 BIO 缓冲**：内存 BIO 与框架缓冲系统无缝对接
4. **安全的资源管理**：完整的生命周期管理和防御机制
5. **透明的版本支持**：自动支持 TLS 1.2 和 TLS 1.3

这套实现方案是生产级别的 HTTPS 处理方案，可作为 C++ 网络框架设计的参考范例。

---

*文档版本：1.0*  
*最后更新：2024年*  
*项目来源：Thunder (Gitee: chenjiayi/thunder)*
