# Thunder 编解码器设计文档

本文档面向开源协作者，说明 `code/Net/src/codec` 下各编解码器的设计目标、协议格式、能力边界与适用场景，帮助快速理解“为什么有这么多 codec”以及“该选哪一个”。

## 1. 设计目标

- 提供统一编解码抽象，屏蔽不同协议在收发细节上的差异。
- 同时支持内部节点通信、客户端私有协议、HTTP/HTTPS、WebSocket 等多种接入形态。
- 在可复用层面下沉压缩与加密能力，避免在业务层重复实现。
- 兼容历史协议，保证存量客户端与新链路可并存演进。

## 2. 架构概览

### 2.1 抽象层

- 基类：`ThunderCodec`
- 核心接口：
  - `Encode(const MsgHead&, const MsgBody&, util::CBuffer*)`
  - `Decode(util::CBuffer*, MsgHead&, MsgBody&)`
  - `Decode(tagConnectionAttr*, MsgHead&, MsgBody&)`
- HTTP 扩展接口（仅 HTTP 类 codec 重写）：
  - `Encode(const HttpMsg&, util::CBuffer*)`
  - `Decode(util::CBuffer*, HttpMsg&)`

### 2.2 协议分层关系

- 业务对象层：`MsgHead/MsgBody`、`HttpMsg`（protobuf 表达）
- 线协议层：Proto 私有格式、Client/App 私有头、HTTP 文本、WebSocket 帧
- 传输安全层：TLS（`HttpsCodec`）

## 3. 编解码器分章节说明

## 3.1 ThunderCodec（基类）

**作用**

- 提供统一 codec 生命周期与接口抽象。
- 提供通用能力函数：`Zip/Unzip`、`Gzip/Gunzip`、`Rc5Encrypt/Decrypt`、`AesEncrypt/Decrypt`、`Aes256Encrypt/Decrypt`。
- 定义命令字高位能力标记常量（如 zip/gzip/rc5/aes 位）。

**不负责的内容**

- 不定义线上报文结构。
- 不直接参与握手与协议文本解析。

**实现说明**

- `Aes256Encrypt/Decrypt` 目前是占位逻辑（透传），并非完整密码学实现。

---

## 3.2 ProtoCodec（内部 PB 通信）

**作用**

- 用于内部节点间的 protobuf 二进制通信。

**报文格式**

- `MsgHead(pb定长)` + `MsgBody(pb字节流)`
- 包体长度以 `MsgHead.msgbody_len` 为准。

**行为特点**

- 编码：序列化头，再序列化体。
- 解码：先解析固定头，再按长度解析体。
- 支持无包体消息（如心跳）。
- 在连接态解码中会根据命令字推进连接状态。

**适用场景**

- 服务间 RPC / 事件转发等内网 PB 链路。

---

## 3.3 ClientMsgCodec（客户端私有头协议）

**作用**

- 面向客户端私有二进制协议（非 HTTP/WS）。

**报文格式**

- `tagClientMsgHead(14B)` + payload
- 头字段：`version(1) | encript(1) | cmd(2) | checksum(2) | body_len(4) | seq(4)`（网络字节序）

**行为特点**

- 命令高位能力位写入 `encript`，低位命令写入 `cmd`。
- payload 默认为 `MsgBody` 的 protobuf 字节流。
- 若命中位标记，可按链路执行 zip/gzip/rc5 的编码与反向解码。
- `body_len == 0` 视为无包体消息。

**适用场景**

- 历史移动端或自定义 TCP 客户端接入。

---

## 3.4 AppMsgCodec（App 独立 16 字节头）

**作用**

- 提供 App 侧定制二进制协议，头结构与 `ClientMsgCodec` 不兼容。

**报文格式**

- `tagAppMsgHead(16B)` + payload
- 头字段：`len(4) | cmd(4) | seq(4) | version(1) | reserve(1) | status(2)`
- 当前启用 `USE_HEAD_LEN`，`len` 语义为“头+体总长”，解码时减去头长。

**行为特点**

- 发送时会构造仅包含 `body` 字段的 `MsgBody` 再序列化。
- `reserve` 位用于区分 AES/RSA 等语义分支（协议注释含登录流程说明）。
- 当 `reserve` 命中 AES 位且存在 session key 时，走对应“aes256”分支。

**适用场景**

- App 协议独立演进、与老客户端头格式隔离的场景。

---

## 3.5 HttpCodec（HTTP/1.x）

**作用**

- 提供标准 HTTP 报文的编码/解析，并桥接到框架统一 `MsgHead/MsgBody`。

**报文格式**

- HTTP 请求/响应文本：
  - 请求行或状态行
  - Header 集
  - Body（可选）

**行为特点**

- `Encode(MsgHead, MsgBody)`：从 `MsgBody.body` 反序列化 `HttpMsg` 后输出 HTTP 文本。
- `Decode(..., MsgHead, MsgBody)`：先解析 HTTP 文本为 `HttpMsg`，再写回 `MsgBody.body`。
- 支持 `Content-Length` 与 `Transfer-Encoding: chunked`。
- 支持 `Content-Encoding: gzip` 的编码与解码还原。
- 使用 `http_parser` 回调驱动解析状态。

**适用场景**

- 标准 HTTP API、回调入口、网关侧明文协议链路。

---

## 3.6 HttpsCodec（TLS + HTTP）

**作用**

- 在 `HttpCodec` 之上增加 TLS 传输安全能力。

**分层设计**

- 明文 HTTP 报文格式复用 `HttpCodec`。
- TLS 握手、加解密由 OpenSSL 驱动（内存 BIO 方案）。

**行为特点**

- 收包：TLS 密文 -> SSL/BIO 解密 -> 明文缓存 -> `HttpCodec::Decode`。
- 发包：`HttpCodec` 先产明文 -> `SSL_write` 加密 -> 密文发送缓冲。
- 按连接 fd 维护独立 `TlsConnState`（角色、握手状态、缓存等）。

**适用场景**

- 需要 HTTPS 接入、证书链路或双向认证扩展的场景。

---

## 3.7 CodecWebSocketJson（WS + JSON 业务体）

**作用**

- WebSocket 通道下承载 JSON 业务体。

**报文格式**

- HTTP 升级握手 -> WebSocket 二进制帧 -> `tagClientMsgHead` + JSON payload

**行为特点**

- JSON 主要承载在 `MsgBody.body` 字段。
- 具备 Base64 辅助能力。
- 握手阶段支持 HTTP 升级识别并切换连接状态。

**适用场景**

- 前端/网关链路偏 JSON 协议生态，调试友好优先。

---

## 3.8 CodecWebSocketPb（WS + protobuf 业务体）

**作用**

- WebSocket 通道下承载 protobuf 业务体。

**报文格式**

- HTTP 升级握手 -> WebSocket 二进制帧 -> `tagClientMsgHead` + `MsgBody(pb)`

**行为特点**

- 相比 Json 版，业务体使用 protobuf 序列化，体积更紧凑。
- 能力位与压缩/加密路径与 `ClientMsgCodec` 语义保持一致。

**适用场景**

- WebSocket 长连接下追求体积与性能的 PB 通信。

---

## 3.9 CodecWebSocketPbApp（WS + App 头）

**作用**

- WebSocket 通道下复用 App 私有头（`tagAppMsgHead`）的协议族。

**报文格式**

- HTTP 升级握手 -> WebSocket 二进制帧 -> `tagAppMsgHead` + `MsgBody(pb)`

**行为特点**

- 头字段与语义跟 `AppMsgCodec` 保持一致。
- 支持 `reserve` 位驱动的 App 侧安全语义（含 session key 路径）。

**适用场景**

- 需要在 WebSocket 接入侧与 App 私有协议保持一致的系统。

---

## 3.10 CodecCustom（测试协议）

**作用**

- 用于测试/验证链路的最小化示例 codec。

**报文格式**

- `clientMsgHead(6B)` + 原始字符串 body
- 头字段：`body_len(2) | seq(4)`

**行为特点**

- 编码固定返回 `"ok"`。
- 解码按长度读取字符串并写入 `MsgBody.body`。

**适用场景**

- 功能冒烟、链路联调、教学示例。

## 4. 选型建议

| 需求 | 推荐 codec | 说明 |
|---|---|---|
| 内网服务 PB 通信 | `ProtoCodec` | 结构简单、开销低、适合节点间 |
| 客户端私有 TCP 协议 | `ClientMsgCodec` | 兼容历史头格式与能力位 |
| App 独立协议演进 | `AppMsgCodec` | 独立头定义，便于单独演进 |
| 标准 HTTP API | `HttpCodec` | 与常见生态兼容 |
| HTTPS 接入 | `HttpsCodec` | 在 HTTP 之上提供 TLS |
| Web 前端 JSON 长连 | `CodecWebSocketJson` | 可读性高、调试方便 |
| WebSocket PB 高性能长连 | `CodecWebSocketPb` | 更紧凑的二进制业务体 |
| WebSocket + App 语义统一 | `CodecWebSocketPbApp` | 保持 App 头与语义一致 |

## 5. 已知边界与注意事项

- `Aes256Encrypt/Decrypt` 当前为占位实现，若用于生产安全链路需先补全真实加解密实现。
- 部分头文件注释存在历史残留（如类名或命名空间注释），请以 `.cpp` 实现逻辑为准。
- 各 codec 在“命令高位/保留位”的语义并非完全统一，新增协议时建议先明确位分配规范。

## 6. 相关源码

- `code/Net/src/codec/ThunderCodec.hpp` / `ThunderCodec.cpp`
- `code/Net/src/codec/ProtoCodec.hpp` / `ProtoCodec.cpp`
- `code/Net/src/codec/ClientMsgCodec.hpp` / `ClientMsgCodec.cpp`
- `code/Net/src/codec/AppMsgCodec.hpp` / `AppMsgCodec.cpp`
- `code/Net/src/codec/HttpCodec.hpp` / `HttpCodec.cpp`
- `code/Net/src/codec/HttpsCodec.hpp` / `HttpsCodec.cpp`
- `code/Net/src/codec/CodecWebSocketJson.hpp` / `CodecWebSocketJson.cpp`
- `code/Net/src/codec/CodecWebSocketPb.hpp` / `CodecWebSocketPb.cpp`
- `code/Net/src/codec/CodecWebSocketPbApp.hpp` / `CodecWebSocketPbApp.cpp`
- `code/Net/src/codec/CodecCustom.hpp` / `CodecCustom.cpp`
