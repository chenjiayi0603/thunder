# 协议编解码器全景

> 覆盖 HTTP、HTTPS、WS、WSS、MQTT、Protobuf 等全部协议。基于源码实测，枚举定义见 `code/Util/src/codec/StreamCodec.hpp`。

---

## 继承树

```
util::CStreamCodec          (枚举: E_CODEC_TYPE)
  └─ net::ThunderCodec      (基类: 压缩/加密/CBuffer 封装)
       ├─ HttpCodec          (HTTP, CODEC_HTTP=3)
       │    └─ HttpsCodec   (HTTPS, CODEC_HTTPS=11)
       ├─ HttpFastCodec      (HTTP Fast-path, 跳过 Body 解析)
       ├─ CodecWebSocketJson (WS JSON, CODEC_WEBSOCKET_EX_JS=5)
       │    └─ WssCodec     (WSS, CODEC_WSS=12)
       ├─ CodecWebSocketPb   (WS Protobuf, CODEC_WEBSOCKET_EX_PB=6)
       ├─ CodecWebSocketPbApp(WS PbApp, CODEC_WEBSOCKET_EX_PB_APP=10)
       ├─ ClientMsgCodec     (私有 TCP 协议, CODEC_PRIVATE=4)
       ├─ AppMsgCodec        (App 协议, CODEC_APP=9)
       ├─ ProtoCodec         (内部 S2S PB, CODEC_PB_INTERNAL=2)
       ├─ CodecMqtt          (MQTT 3.1.1, CODEC_MQTT=13)
       └─ CodecCustom        (自定义)
```

---

## E_CODEC_TYPE 枚举 (完整)

```cpp
enum E_CODEC_TYPE {
    CODEC_UNKNOW             = 0,   // 未知
    CODEC_PB_INTERNAL        = 2,   // 内部 S2S Protobuf
    CODEC_HTTP               = 3,   // HTTP/1.1
    CODEC_PRIVATE            = 4,   // 私有 TCP 协议
    CODEC_WEBSOCKET_EX_JS    = 5,   // WS JSON
    CODEC_WEBSOCKET_EX_PB    = 6,   // WS Protobuf
    CODEC_TLV                = 7,   // TLV 格式
    CODEC_TEST               = 8,   // 测试用
    CODEC_APP                = 9,   // App 协议
    CODEC_WEBSOCKET_EX_PB_APP = 10, // WS Protobuf App
    CODEC_HTTPS              = 11,  // HTTP over TLS
    CODEC_WSS                = 12,  // WS over TLS
    CODEC_MQTT               = 13,  // MQTT 3.1.1
};
```

---

## 通用状态机

| 状态 | 值 | 含义 |
|------|----|------|
| `CODEC_STATUS_OK` | 0 | 解码成功，可分发到业务层 |
| `CODEC_STATUS_ERR` | 1 | 协议错误，关闭连接 |
| `CODEC_STATUS_PAUSE` | 2 | 数据不完整，等待下一轮 IO |

---

## HTTP 编解码 (picohttpparser)

```
HttpCodec::Decode(pRecvBuff):
  1. phr_parse_request(buf, len, &method, &path, &minor_version, headers)
     → SSE4.2 SIMD 加速 — 一次比较 16 字节
  2. Content-Length → body_len
  3. Transfer-Encoding: chunked → 逐 chunk 解析
  4. body 完整 → CODEC_STATUS_OK
  5. body 不完整 → CODEC_STATUS_PAUSE
```

### HttpFastCodec (跳过 Body)

`HttpFastCodec` 继承 `HttpCodec`，收到完整 Header 后直接返回 `CODEC_STATUS_OK`，
**不等待 Body**。用于 `/hello/raw` 等不需要读取请求体的端点，减少内存拷贝和解析开销。

```
HttpFastCodec::Decode:
  1. phr_parse_request (仅 Header)
  2. Header 完整 → CODEC_STATUS_OK (丢弃后续 Body)
```

---

## HTTPS / TLS 编解码

> `HttpsCodec` = `HttpCodec` + OpenSSL TLS 层。HTTP 逻辑完全复用基类，HttpsCodec 只增加加解密。

### TLS 握手

```
IoCallback (EV_READ)
  → IoRead → RecvDataAndDispose
    → codec->Decode(pConn)
      → SSL_is_init_finished == false
        → SSL_do_handshake()  (非阻塞)
          → 需要更多数据 → CODEC_STATUS_PAUSE
          → 握手完成 → SSL_is_init_finished = true
      → SSL_read() → 解密 → HttpCodec::Decode() 复用基类解析
```

### 数据流

```
客户端 TCP → RecvDataAndDispose → HttpsCodec::Decode
  ├─ TLS 未握手 → SSL_do_handshake() → CODEC_STATUS_PAUSE
  └─ TLS 已握手 → SSL_read() 解密 → HttpCodec::Decode → MsgHead+MsgBody → Dispose

响应:
  HttpCodec::Encode(MsgHead, MsgBody) → HttpsCodec::Encode → SSL_write() 加密 → TCP send
```

### HTTP vs HTTPS 连接建立

```
配置: "codec_type": "HTTP" / "HTTPS"
→ AcceptClientConn → CreateCodec(eCodecType)
    HTTP:  new HttpCodec
    HTTPS: new HttpsCodec(SSL_CTX) → SSL_new → SSL_set_fd
```

业务代码完全无感知 — `MsgHead + MsgBody` 接口一致。

---

## WebSocket 帧格式

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+-------------------------------+
|     Masking-key (if MASK=1)   |       Payload Data            |
+-------------------------------+-------------------------------+
```

### CodecWebSocketJson

```
Decode: ReadFrame → unmask payload → JSON parse → fill HttpMsg → AnyMessage(Module)
```

### CodecWebSocketPb

```
Decode: ReadFrame → unmask payload → Protobuf unpack → MsgHead+MsgBody → Dispose
```

WSS = WS 帧格式 + TLS。`WssCodec` 继承 `CodecWebSocketJson` 叠加 `SSL_do_handshake`，与 HTTPS 模式一致。

---

## 内部 Protobuf (S2S)

```
ProtoCodec (CODEC_PB_INTERNAL=2):

帧格式:
  ┌──────────┬───────────┬───────────┐
  │ 4B 总长度 │ N B head  │ M B body  │
  │ (网络序)  │ (PB序列化) │ (PB序列化) │
  └──────────┴───────────┴───────────┘

Decode:
  1. ReadUint32 → total_len
  2. ReadableBytes < total_len+4 → CODEC_STATUS_PAUSE
  3. head_len = ReadUint32
  4. head.ParseFromArray(buf+8, head_len)
  5. body.ParseFromArray(buf+8+head_len, total_len-head_len)
  6. CODEC_STATUS_OK
```

一个 TCP 连接支持多路复用 (seq 匹配):
```
连接 → [seq:1] LOGIC → [seq:2] LOGIC → [seq:3] LOGIC
        ← [seq:2] resp                ← [seq:1] resp
```

---

## MQTT 3.1.1 (IoT)

```
CodecMqtt (CODEC_MQTT=13):

固定头: 2-5 字节 (type+flags+remaining_length)
剩余长度 → 可变头 + payload

Decode:
  1. read_byte → type + flags
  2. read remaining_length (变长编码, 1-4 字节)
  3. 等待数据到达 remaining_length 字节 → CODEC_STATUS_PAUSE
  4. 根据 type 分派 (CONNECT/PUBLISH/SUBSCRIBE/...)
  5. CODEC_STATUS_OK
```
