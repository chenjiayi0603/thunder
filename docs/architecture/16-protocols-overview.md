# 协议编解码器全景

> 覆盖 HTTP、HTTPS、WS、WSS 及其他所有协议，基于源码实测

---

## 继承树

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

---

## 通用状态机

所有编解码器共享同一套返回值语义：

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

### CodecWebSocketJson 流程

```
Decode(recvBuff):
  1. ReadFrame → FIN + opcode + payload_len + mask → payload[] unmask
  2. payload → JSON parse → fill HttpMsg (path/method/body)
  3. CODEC_STATUS_OK → AnyMessage(Module)
```

### CodecWebSocketPb 流程

```
Decode(recvBuff):
  1. ReadFrame → unmask payload
  2. Protobuf unpack → MsgHead + MsgBody (cmd/seq/body)
  3. CODEC_STATUS_OK → Dispose(Step/Cmd)
```

WSS = WS 帧格式 + TLS 层（OpenSSL），`WssCodec` 继承 `CodecWebSocketJson` 叠加 TLS 握手。

---

## 内部 Protobuf 协议 (S2S)

```
ProtoCodec (CODEC_PB_INTERNAL=2):

帧格式:
  ┌──────────┬───────────┬───────────┐
  │ 4B 总长度 │ N B head  │ M B body  │
  │ (网络序)  │ (PB序列化) │ (PB序列化) │
  └──────────┴───────────┴───────────┘

Decode(CBuffer):
  1. ReadUint32 → total_len
  2. if ReadableBytes < total_len + 4 → CODEC_STATUS_PAUSE
  3. head_len = ReadUint32
  4. head.ParseFromArray(buf+8, head_len)
  5. body.ParseFromArray(buf+8+head_len, total_len-head_len)
  6. CODEC_STATUS_OK
```

### 多路复用

一个 TCP 连接可承载多个并发请求（seq 匹配）：
```
连接 → [seq:1] LOGIC → [seq:2] LOGIC → [seq:3] LOGIC
        ← [seq:2] resp                ← [seq:1] resp
```
