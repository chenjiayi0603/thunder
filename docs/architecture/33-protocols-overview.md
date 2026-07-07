# Thunder 协议编解码器全景分析

> 覆盖 HTTP、HTTPS、WS（WebSocket）、WSS 及其他所有协议，基于源码实测。

---

## 一、编解码器体系总览

### 1.1 继承树

```
util::CStreamCodec          (枚举: E_CODEC_TYPE)
  └─ net::ThunderCodec      (基类: 压缩/加密/状态机)
       ├─ HttpCodec          (HTTP, CODEC_HTTP=3)
       │    └─ HttpsCodec   (HTTPS, CODEC_HTTPS=11)
       ├─ CodecWebSocketJson (WS JSON, CODEC_WEBSOCKET_EX_JS=5)
       │    └─ WssCodec     (WSS, CODEC_WSS=12)
       ├─ CodecWebSocketPb   (WS Protobuf, CODEC_WEBSOCKET_EX_PB=6)
       ├─ CodecWebSocketPbApp(WS PbApp, CODEC_WEBSOCKET_EX_PB_APP=10)
       ├─ ClientMsgCodec     (私有协议, CODEC_PRIVATE=4)
       ├─ AppMsgCodec        (App 协议, CODEC_APP=9)
       ├─ ProtoCodec         (内部 PB, CODEC_PB_INTERNAL=2)
       └─ CodecCustom        (自定义)
```

### 1.2 编解码器枚举一览

```cpp
enum E_CODEC_TYPE {
    CODEC_UNKNOW              = 0,
    CODEC_PB_INTERNAL         = 2,   // 内部节点间 Protobuf 通信
    CODEC_HTTP                = 3,   // HTTP 明文
    CODEC_PRIVATE             = 4,   // 私有 TCP 协议（客户端接入）
    CODEC_WEBSOCKET_EX_JS     = 5,   // WS + JSON 消息体
    CODEC_WEBSOCKET_EX_PB     = 6,   // WS + Protobuf 消息体
    CODEC_TLV                 = 7,   // TLV 编码
    CODEC_TEST                = 8,   // 测试用
    CODEC_APP                 = 9,   // App 侧私有协议
    CODEC_WEBSOCKET_EX_PB_APP = 10,  // WS + PbApp（AES/RSA）
    CODEC_HTTPS               = 11,  // HTTPS（HTTP over TLS）
    CODEC_WSS                 = 12,  // WSS（WebSocket over TLS）
};
```

### 1.3 通用状态机

所有编解码器共享同一套返回值语义：

| 状态 | 值 | 含义 |
|------|----|------|
| `CODEC_STATUS_OK` | 0 | 成功，数据完整解析 |
| `CODEC_STATUS_ERR` | 1 | 错误，调用方应关闭连接 |
| `CODEC_STATUS_PAUSE` | 2 | 数据不足或握手等待，继续监听 |

### 1.4 ThunderCodec 基类能力

```
发送方向 (Encode):
  MsgHead + MsgBody
       │
       ▼ 序列化（Protobuf）
       │
       ▼ 压缩（可选: zlib/gzip, 由 cmd 高位控制）
       │    gc_uiZipBit  = 0x20000000
       │    gc_uiGzipBit = 0x10000000
       │
       ▼ 加密（可选: RC5-12轮/AES-128, 由 cmd 高位控制）
       │    gc_uiRc5Bit  = 0x01000000
       │    gc_uiAesBit  = 0x02000000
       │
       ▼ CBuffer（发送缓冲）

接收方向 (Decode): 逆序 解密→解压→反序列化
```

---

## 二、HTTP（CODEC_HTTP = 3）

**实现**：`code/Net/src/codec/HttpCodec.hpp/cpp`

**适用场景**：服务间或客户端与服务端的标准 HTTP/1.1 通信，明文传输。

### 2.1 工作原理

```
客户端 HTTP 请求（明文字节流）
       │
       ▼ HttpCodec::Decode(pBuff, oHttpMsg)
  http_parser_execute()
  ├─ OnUrl()        → 解析请求路径
  ├─ OnHeaderField()/OnHeaderValue() → 填充 headers map
  ├─ OnBody()       → 累加 body
  └─ OnMessageComplete() → 标记解码完成，is_decoding=false
       │
       ▼ HttpMsg（protobuf）
       │
  业务处理
       │
       ▼ HttpCodec::Encode(oHttpMsg, pBuff)
  组装: 状态行 + Headers + 空行 + Body
```

### 2.2 连接级优化：Protobuf Arena

```cpp
struct HttpConnContext {
    google::protobuf::Arena arena;  // 8KB 预分配，每次请求后 Reset()
};
// 挂在 tagConnectionAttr::pProtoCtx，连接关闭时 delete
```

Arena 预分配减少 HttpMsg 反复 new/delete 的堆碎片，单连接长期复用。

### 2.3 Body 传输策略

| 大小 | 编码方式 |
|------|---------|
| ≤ 8192 字节 | `Content-Length: N` |
| > 8192 字节 | `Transfer-Encoding: chunked`（8192/块） |
| gzip 压缩后 ≤ 8192 | `Content-Length: N` |
| gzip 压缩后 > 8192 | `Transfer-Encoding: chunked` |

chunked 格式：
```
长度(hex)\r\n
数据\r\n
...
0\r\n\r\n
```

### 2.4 典型配置

```json
{
  "access_codec": 3,
  "access_port": 27006,
  "inner_port": 27007
}
```

---

## 三、HTTPS（CODEC_HTTPS = 11）

**实现**：`code/Net/src/codec/HttpsCodec.hpp/cpp`

**适用场景**：HTTP over TLS，对外提供安全 REST 接口，支持单向/双向证书认证。

### 3.1 关键设计：HTTPS = HTTP + TLS（正交组合）

```
HttpsCodec 只增加 TLS 层，HTTP 逻辑完全复用 HttpCodec：

  [TLS 层]  OpenSSL BIO 内存缓冲，握手状态机，SSL_read/SSL_write
       │                │
       ▼                ▼
  [HTTP 层]  HttpCodec::Decode / HttpCodec::Encode（原封不动复用）
```

### 3.2 每连接 TLS 状态（TlsConnState）

```cpp
struct TlsConnState {
    SSL_CTX* pCtx;               // SSL 上下文（证书/密钥）
    SSL*     pSsl;               // SSL 连接（每 fd 独立）
    bool     bServerSide;        // 角色：服务端/客户端
    bool     bHandshakeDone;     // 握手是否完成
    CBuffer  oPlainRecvBuff;     // 解密后的明文
    CBuffer  oPendingPlainSend;  // 握手期暂存待发明文
};
```

### 3.3 读路径（密文 → HTTP）

```
pConn->pRecvBuff（网络密文）
    │
    ▼ FeedCipherToSsl: BIO_write → RBIO
    │
    ▼ DoHandshake: SSL_do_handshake（可多轮，WANT_READ/WANT_WRITE → PAUSE）
    │
    ▼ DrainSslToPlain: SSL_read 循环 → oPlainRecvBuff
    │
    ▼ HttpCodec::Decode(&oPlainRecvBuff, ...) → MsgHead + MsgBody
```

### 3.4 写路径（HTTP → 密文）

```
MsgHead + MsgBody
    │
    ▼ HttpCodec::Encode → oPlainBuff
    │
    ▼ EncryptPlain:
      ├─ 握手未完成 → 写 oPendingPlainSend，返回 PAUSE
      └─ 握手完成  → SSL_write(pSsl, ...)
    │
    ▼ DrainOutboundCipher: BIO_read(WBIO) → pConn->pSendBuff
```

### 3.5 TLS 握手驱动（应用层驱动模型）

每次 Decode 调用都尝试推进握手，与协程模型天然契合：

```
SSL_do_handshake()
  ├─ ret==1: bHandshakeDone=true，进入正常收发
  ├─ WANT_READ: 需要更多网络输入，返回 PAUSE 继续监听
  ├─ WANT_WRITE: 需要发送握手包，DrainOutboundCipher 后返回 PAUSE
  └─ 其他: 握手失败，返回 ERR
```

握手期间：服务端发送的握手密文通过 `DrainOutboundCipher` 实时刷入 `pSendBuff`，等下一次 IO 写事件统一发出。

### 3.6 TLS 版本支持

```cpp
// 自动选择最高可用版本（TLS 1.2 / TLS 1.3）
SSL_CTX* pCtx = SSL_CTX_new(
    bServerSide ? TLS_server_method() : TLS_client_method()
);
```

### 3.7 配置与端口

```json
{ "access_codec": 11, "access_port": 27443, "inner_port": 27444 }
```

详见 `docs/architecture/32-https-codec.md`（完整运维指南）。

---

## 四、WebSocket（WS）：三种变体

WebSocket 在 Thunder 中有三种编解码器，共享相同的帧格式，但消息头和业务体结构不同。

### 4.1 共同点：WebSocket 帧结构

所有 WS 变体都使用标准 RFC 6455 帧格式：

```
字节 0        字节 1         字节 2-N
┌─────────┬─────────────┬──────────────────┐
│FIN|RSV|OP│MASK|PayLen  │Extended+Mask+Data│
└─────────┴─────────────┴──────────────────┘

FIN(1b) + RSV(3b) + Opcode(4b):
  0x0=续帧  0x1=文本  0x2=二进制  0x8=关闭
  0x9=ping  0xA=pong

MASK(1b) + PayloadLen(7b):
  <126: 直接长度
  126:  后跟 2 字节 uint16 (网络序)
  127:  后跟 8 字节 uint64 (网络序)

客户端→服务端：MASK 位必须为 1，含 4 字节掩码
服务端→客户端：MASK 位为 0，无掩码
```

### 4.2 共同点：HTTP Upgrade 握手流程

WS 连接建立的第一阶段是 HTTP Upgrade：

```
初始状态 (eConnectStatus_init):

客户端发 HTTP GET (含 Upgrade: websocket):
  GET /ws HTTP/1.1
  Upgrade: websocket
  Connection: Upgrade
  Sec-WebSocket-Key: <base64>
  Sec-WebSocket-Version: 13

服务端解码 → 识别 Upgrade 头 → ucConnectStatus = eConnectStatus_ok
服务端回应 101 Switching Protocols

后续所有通信走 WebSocket 二进制帧
```

`CodecWebSocketJson::Decode(tagConnectionAttr*, ...)` 中的切换逻辑：

```cpp
if (pConn->ucConnectStatus == eConnectStatus_init) {
    // 检测 GET/POST 开头
    if (memcmp(pRaw, "GET ", 4) == 0 || memcmp(pRaw, "POST ", 5) == 0) {
        Decode(pBuff, oHttpMsg);  // 先用 HTTP 解析
        if (Upgrade: websocket) {
            pConn->ucConnectStatus = eConnectStatus_ok;
            oMsgBody.set_body(oHttpMsg.SerializeAsString()); // 把 HttpMsg 传给业务
        }
    }
}
// 状态变为 ok 后，后续走 WS 帧解码
return Decode(pConn->pRecvBuff.get(), oMsgHead, oMsgBody);
```

---

### 4.3 CODEC_WEBSOCKET_EX_JS（值=5）

**实现**：`CodecWebSocketJson`

**消息头结构**（WS Payload 内嵌）：

```cpp
struct tagClientMsgHead {
    uint8  version;    // 版本（暂时无用）
    uint8  encript;    // cmd 高8位（加密/压缩标志）
    uint16 cmd;        // 命令字（网络序）
    uint32 body_len;   // 消息体长度（网络序）
    uint32 seq;        // 序列号（网络序）
    uint16 checksum;   // 校验码（发送端填0）
};
```

**Payload 布局**：

```
[WS 帧头] [tagClientMsgHead(13字节)] [业务 JSON 字符串]
```

**业务体**：`MsgBody.body` 存放原始 JSON 字符串（UTF-8），不做 protobuf 序列化。

**压缩/加密**：由 `oMsgHead.cmd()` 高位控制（`gc_uiZipBit`、`gc_uiGzipBit`、`gc_uiRc5Bit`），编码时按 "压缩→加密" 顺序，解码时按 "解密→解压" 顺序。

**掩码处理**（客户端→服务端方向）：

```cpp
// 解码时原地 XOR 去掩码
for (int j = 0; j < uiPayload; ++j) {
    pRawData[j] ^= szMaskKey[j % 4];
}
```

---

### 4.4 CODEC_WEBSOCKET_EX_PB（值=6）

**实现**：`CodecWebSocketPb`

与 CODEC_WEBSOCKET_EX_JS 的区别：

| 维度 | WEBSOCKET_EX_JS | WEBSOCKET_EX_PB |
|------|-----------------|-----------------|
| 消息头 | `tagClientMsgHead` | `tagClientMsgHead` |
| 业务体编码 | JSON 字符串 (`body` 字段) | `MsgBody` 的 Protobuf 序列化 |
| 解码输出 | `MsgBody.body = JSON字符串` | `MsgBody` 完整反序列化 |
| 适用场景 | Web 前端/移动端（JSON友好） | 高效内部/APP 通信 |

其余帧格式、Upgrade 握手、压缩/加密机制与 WEBSOCKET_EX_JS 完全相同。

---

### 4.5 CODEC_WEBSOCKET_EX_PB_APP（值=10）

**实现**：`CodecWebSocketPbApp`

这是面向 App 端（移动客户端）的变体，与前两者的核心差异：

**消息头结构**（`tagAppMsgHead`，区别于 `tagClientMsgHead`）：

```cpp
struct tagAppMsgHead {
    uint32 len;        // 总包长
    uint32 cmd;        // 命令字
    uint32 seq;        // 序列号
    uint32 version;    // 协议版本
    uint32 reserve;    // 预留位（高位承载加密/版本信息）
    uint32 status;     // 状态码
};
```

**加密方案**：结合 `reserve` 字段标志位，走 App 侧 AES-256/RSA 约定（`gc_app_Aes_CmdBit=0x04000000`、`gc_app_Rsa_CmdBit=0x08000000`），与 WS_EX_JS/PB 的 RC5 方案不同。

**握手处理**：通过 `DecodeHandShake` 单独处理，当前实现侧重 GET 方法。

---

## 五、WSS（CODEC_WSS = 12）

**实现**：`code/Net/src/codec/WssCodec.hpp/cpp`

**适用场景**：WebSocket over TLS，安全的实时双向通信。

### 5.1 设计思路：WSS = WS + TLS（与 HTTPS 同构）

```
WssCodec 继承 CodecWebSocketJson（复用 WS 帧逻辑）
  +
TLS 层实现与 HttpsCodec 几乎一致（共用相同的 BIO 内存缓冲模型）
```

`WssCodec` 与 `HttpsCodec` 的 TLS 实现完全平行（结构体名相同，逻辑相同），只是上层协议从 HTTP 换成了 WebSocket。

### 5.2 Decode 流程（六步）

```cpp
E_CODEC_STATUS WssCodec::Decode(tagConnectionAttr* pConn, ...) {
    // 1. 网络密文 → RBIO
    FeedCipherToSsl(pConn, pState);

    // 2. 推进 TLS 握手
    DoHandshake(pConn, pState, pConn->pSendBuff.get());
    // PAUSE → 等待握手完成

    // 3. 握手完成后，发送暂存的待发明文（如服务端 101 响应）
    if (oPendingPlainSend.ReadableBytes() > 0) {
        EncryptPlain(..., oPendingPlainSend, pConn->pSendBuff.get());
        oPendingPlainSend.SkipBytes(...);
    }

    // 4. SSL_read 取明文 → oPlainRecvBuff
    DrainSslToPlain(pState);

    // 5. 初始状态：HTTP Upgrade 握手（从 oPlainRecvBuff 读，而非 pRecvBuff）
    if (pConn->ucConnectStatus == eConnectStatus_init) {
        // 检测 GET/POST，解析 HTTP，确认 Upgrade: websocket
        pConn->ucConnectStatus = eConnectStatus_ok;
    }

    // 6. 握手完成后：WS 帧解码
    return CodecWebSocketJson::Decode(&pState->oPlainRecvBuff, oMsgHead, oMsgBody);
}
```

### 5.3 与 WS 的关键区别

| 维度 | WS（WEBSOCKET_EX_JS=5） | WSS（CODEC_WSS=12） |
|------|--------------------------|----------------------|
| 传输层 | TCP 明文 | TLS 加密 |
| HTTP Upgrade 数据源 | `pConn->pRecvBuff` | `oPlainRecvBuff`（TLS解密后） |
| WS 帧来源 | `pConn->pRecvBuff` | `oPlainRecvBuff` |
| 连接角色设置 | 无 | `SetConnectionRole(fd, bServerSide)` |
| 资源清理 | 无额外操作 | `RemoveConnection(fd)` 释放 SSL/SSL_CTX |

### 5.4 TLS 层实现（与 HTTPS 一致）

```
密文 pRecvBuff → BIO_write(RBIO) → SSL引擎 → BIO_read(WBIO) → pSendBuff
                                      │
                                   SSL_read → oPlainRecvBuff（WS帧明文）
```

EnsureState 中的角色判断（WSS 默认客户端，与 HTTPS 默认服务端相反）：

```cpp
// WssCodec::EnsureState
const bool bServerSide = (m_mapConnRole.find(fd) != end) ? m_mapConnRole[fd] : false;
//                                                                              ↑ 默认客户端
// HttpsCodec::EnsureState
const bool bServerSide = (roleIt != end) ? roleIt->second : true;
//                                                           ↑ 默认服务端
```

### 5.5 WSS 配置

```cpp
WssCodec::SslConfig cfg;
cfg.strServerCertFile = "conf/certs/server.crt";
cfg.strServerKeyFile  = "conf/certs/server.key";
cfg.strServerCaFile   = "conf/certs/ca.crt";
cfg.bServerVerifyClient = false;
m_pWssCodec->SetSslConfig(cfg);
```

---

## 六、协议横向对比

### 6.1 功能矩阵

| 协议 | 枚举值 | TLS | 全双工 | 消息格式 | 适用场景 |
|------|:------:|:---:|:------:|---------|---------|
| HTTP | 3 | ✗ | ✗ | HTTP/1.1 请求响应 | REST API、管理接口 |
| HTTPS | 11 | ✓ | ✗ | HTTP/1.1 over TLS | 对外安全 REST API |
| WS-JSON | 5 | ✗ | ✓ | WS帧 + tagClientMsgHead + JSON | Web/移动端实时通信 |
| WS-PB | 6 | ✗ | ✓ | WS帧 + tagClientMsgHead + PB | 高效实时通信 |
| WS-PbApp | 10 | ✗ | ✓ | WS帧 + tagAppMsgHead + AES/RSA | App端安全通信 |
| WSS | 12 | ✓ | ✓ | WS帧 over TLS | 安全实时双向通信 |
| PB Internal | 2 | ✗ | ✓ | Protobuf | 内部服务间通信 |

### 6.2 TLS 层对比（HTTPS vs WSS）

两者 TLS 实现高度同构，差异仅在上层协议：

| 维度 | HTTPS（HttpsCodec） | WSS（WssCodec） |
|------|---------------------|-----------------|
| 继承基类 | HttpCodec | CodecWebSocketJson |
| 上层协议 | HTTP/1.1 | WebSocket 帧 |
| 默认角色 | 服务端（true） | 客户端（false） |
| 握手后数据解析 | HttpCodec::Decode | CodecWebSocketJson::Decode |
| 帧初始化检测 | HTTP 请求行 | HTTP GET/POST（WS Upgrade） |
| TLS 实现 | 完全一致 | 完全一致 |

### 6.3 WebSocket 三种变体消息头对比

```
WEBSOCKET_EX_JS / WEBSOCKET_EX_PB (tagClientMsgHead, 13字节):
  [1:version][1:encript][2:cmd][4:body_len][4:seq][2:checksum]
  ↓
  WS Payload = tagClientMsgHead + 业务体
  业务体区别: JS=JSON字符串  PB=MsgBody.SerializeToString()

WEBSOCKET_EX_PB_APP (tagAppMsgHead, 24字节):
  [4:len][4:cmd][4:seq][4:version][4:reserve][4:status]
  ↓
  加密方案: AES-256/RSA（reserve 字段标志位控制）
```

### 6.4 连接状态转换（WS/WSS 特有）

```
eConnectStatus_init
    │
    │  解码到 HTTP GET + Upgrade: websocket
    ▼
eConnectStatus_ok   (后续所有帧走 WS 二进制帧解码)
```

HTTP/HTTPS 连接无此状态，每个请求独立解析。

---

## 七、IO 模型与编解码器协作

### 7.1 事件驱动通用模型

```
Epoll/io_uring 可读事件
    │
    ▼ Worker::IoRead
  pConn->pRecvBuff 接收网络字节
    │
    ▼ Codec::Decode(pConn, oMsgHead, oMsgBody)
  ├─ HTTP/HTTPS: 直接或经 TLS 解密后调 http_parser
  ├─ WS/WSS:    状态判断 + WS 帧解码（WSS 含 TLS 解密前置）
  └─ 返回 OK → 分发给业务层
     返回 PAUSE → 继续监听（等数据完整或握手完成）
     返回 ERR → 关闭连接

Epoll/io_uring 可写事件
    │
    ▼ Worker::IoWrite
  pConn->pSendBuff 发送到网络
```

### 7.2 TLS 双层 BIO 的必要性

HTTP 和 WS 可以直接读写 `pConn->pRecvBuff`，但 HTTPS 和 WSS 必须通过内存 BIO 中间层：

```
不用 BIO（错误设计）:
  OS → pRecvBuff → SSL_read(fd)   ← fd 已被框架管理，OpenSSL 不能直接 read

使用内存 BIO（正确设计）:
  OS → pRecvBuff → BIO_write(RBIO) → SSL 状态机 → SSL_read() → 明文
  明文 → SSL_write() → BIO_read(WBIO) → pSendBuff → OS
```

### 7.3 非阻塞 IO 下的 WANT_READ/WANT_WRITE

```
SSL_do_handshake / SSL_read / SSL_write 返回 ≤ 0 时:

SSL_ERROR_WANT_READ:
  SSL 引擎需要更多网络输入才能继续
  → 框架：返回 PAUSE，等下一次可读事件，再调 FeedCipherToSsl

SSL_ERROR_WANT_WRITE:
  SSL 引擎有数据需要发出去才能继续
  → 框架：调 DrainOutboundCipher 刷出密文，返回 PAUSE，等可写事件

SSL_ERROR_ZERO_RETURN:
  对端发送了 close_notify（优雅关闭）
  → 框架：返回 ERR，关闭连接

其他错误:
  → 框架：返回 ERR，关闭连接
```

---

## 八、选型建议

| 需求 | 推荐协议 | 原因 |
|------|---------|------|
| 对外 REST API（开发/内网） | HTTP (3) | 简单，无证书管理成本 |
| 对外 REST API（生产/公网） | HTTPS (11) | TLS 安全，复用 HTTP 业务逻辑 |
| 浏览器实时通信 | WS-JSON (5) | JSON 天然兼容前端，Upgrade 握手标准 |
| 移动端/高效通信 | WS-PB (6) | Protobuf 比 JSON 小 30-50% |
| App 端（需加密） | WS-PbApp (10) | AES/RSA，适合 App 私有协议 |
| 安全实时通信 | WSS (12) | WS 全双工 + TLS 加密，公网安全 |
| 内部服务间 | PB Internal (2) | 最高效，无需 HTTP 开销 |

---

## 九、关键文件索引

| 文件 | 路径 | 协议 |
|------|------|------|
| `StreamCodec.hpp` | `code/Util/src/util/` | 枚举定义 |
| `ThunderCodec.hpp/cpp` | `code/Net/src/codec/` | 公共基类 |
| `HttpCodec.hpp/cpp` | `code/Net/src/codec/` | HTTP |
| `HttpsCodec.hpp/cpp` | `code/Net/src/codec/` | HTTPS |
| `CodecWebSocketJson.hpp/cpp` | `code/Net/src/codec/` | WS-JSON |
| `CodecWebSocketPb.hpp/cpp` | `code/Net/src/codec/` | WS-PB |
| `CodecWebSocketPbApp.hpp/cpp` | `code/Net/src/codec/` | WS-PbApp |
| `WssCodec.hpp/cpp` | `code/Net/src/codec/` | WSS |
| `ClientMsgHead.hpp` | `code/Net/src/codec/` | tagClientMsgHead 结构 |

**相关文档**：
- `docs/architecture/32-https-codec.md` — HTTPS 完整运维指南（含证书、部署、排障）
