# HTTPS/TLS 编解码器

> 源码: `code/Util/src/util/StreamCodec.hpp`, `code/Net/src/codec/HttpsCodec.cpp`

---

## 继承体系

```
CStreamCodec          (枚举定义: E_CODEC_TYPE)
  └─ ThunderCodec     (基类: 压缩/加密/CBuffer 封装)
       └─ HttpCodec   (HTTP 明文: http_parser 集成)
            └─ HttpsCodec  (HTTPS = HTTP + TLS)
```

每层只关注自身职责：HttpCodec 可独立于 TLS 运行；HttpsCodec 只增加 TLS 加解密层，HTTP 逻辑完全复用基类。

---

## E_CODEC_TYPE 枚举

```cpp
enum E_CODEC_TYPE {
    CODEC_HTTP               = 3,
    CODEC_HTTPS              = 11,
    CODEC_WEBSOCKET_EX_JS    = 5,   // WS JSON
    CODEC_WEBSOCKET_EX_PB    = 6,   // WS Protobuf
    CODEC_WEBSOCKET_EX_PB_APP = 10,
    CODEC_WSS                = 12,  // WSS = WS over TLS
    CODEC_PB_INTERNAL        = 2,   // 内部节点间 PB 通信
    CODEC_PRIVATE            = 4,   // 私有 TCP 协议
};
```

---

## E_CODEC_STATUS 状态机

```cpp
enum E_CODEC_STATUS {
    CODEC_STATUS_OK    = 0,  // 成功
    CODEC_STATUS_ERR   = 1,  // 错误（关闭连接）
    CODEC_STATUS_PAUSE = 2,  // 数据不完整或握手未完成，等待下一轮 IO
};
```

---

## TLS 握手流程

```
IoCallback (EV_READ)
  → IoRead → RecvDataAndDispose
    → pConn->eCodecType == CODEC_HTTPS
    → codec->Decode(pConn)
      → SSL_is_init_finished = false
        → SSL_do_handshake() ← 非阻塞
          → 需要更多数据 → CODEC_STATUS_PAUSE
          → 握手完成 → SSL_is_init_finished = true
      → SSL_read() → 解密 → HTTP 解析 → 复用 HttpCodec::Decode
```

---

## HTTPS 数据流

```
客户端 TCP 数据
  → RecvDataAndDispose (读入 pRecvBuff)
  → HttpsCodec::Decode
       │
       ├─ TLS 未握手 → SSL_do_handshake() → CODEC_STATUS_PAUSE 等待更多握手数据
       │
       └─ TLS 已握手
            └─ SSL_read() 解密
                 └─ 解密后数据 → 复用 HttpCodec 解析 HTTP
                      ├─ HttpCodec::Decode → MsgHead + MsgBody
                      └─ Dispose(Step/Cmd)

响应:
  HttpCodec::Encode(MsgHead, MsgBody) → HTTP 响应字节流
    → HttpsCodec::Encode → SSL_write() 加密
    → pSendBuff → IoWrite → TCP send
```

---

## HTTP vs HTTPS 分离设计

```
配置: "codec_type": "HTTP" / "HTTPS"
连接建立: AcceptClientConn → CreateCodec(eCodecType)
  HTTP:  new HttpCodec
  HTTPS: new HttpsCodec(SSL_CTX)
          → SSL_new → SSL_set_fd

业务代码无感知: MsgHead + MsgBody 接口完全一致
```
