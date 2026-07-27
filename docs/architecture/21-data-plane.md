# 数据面 — 网络 I/O · 编解码管道

> 源码: `code/Net/src/labor/Worker.cpp` (IoRead/IoWrite), `code/Util/src/util/`

---

## 读数据流程

```
epoll_wait → EV_READ
  │
  ▼
IoCallback → IoRead → RecvDataAndDispose
  │
  ├─ pRecvBuff->Compact(8192)      ← 释放已读空间
  ├─ pRecvBuff->ReadFD(fd, err)    ← 非阻塞 read
  │
  ▼
  while (ReadableBytes >= MsgHeadSize):
    ├─ codec->Decode(pConn, oMsgHead, oMsgBody)
    │   ├─ CODEC_STATUS_OK:
    │   │   ├─ Protocol message (cmd > 0) → Dispose(Step/Cmd)
    │   │   └─ HTTP/WS message → Dispose(HttpMsg)
    │   │       └─ Encode response → pSendBuff->WriteFD → Compact(8192)
    │   ├─ CODEC_STATUS_PAUSE: break  ← 数据不完整
    │   └─ CODEC_STATUS_ERR: DestroyConnect
    │
    ▼
  return true;

错误处理:
  read == 0        → 对端关闭 → DestroyConnect
  errno == EAGAIN  → 正常（非阻塞读空）
  errno == EINTR   → goto read_again
  errno == other   → DestroyConnect
```

---

## 编解码器体系

```
ThunderCodec (抽象基类)
  ├─ Encode(MsgHead, MsgBody) → CBuffer
  ├─ Decode(CBuffer) → MsgHead, MsgBody
  │
  ├─ ProtoCodec          ← S2S 内部通信（MsgHead+MsgBody 二进制）
  ├─ HttpCodec           ← HTTP 请求/响应解析 (picohttpparser)
  ├─ HttpsCodec          ← HTTPS（OpenSSL 握手 + HTTP）
  ├─ CodecWebSocketJson  ← WebSocket JSON 帧
  ├─ CodecWebSocketPb    ← WebSocket Protobuf 帧
  ├─ CodecWebSocketPbApp ← WebSocket Protobuf App 帧（带用户会话）
  ├─ ClientMsgCodec      ← 客户端私有协议
  ├─ AppMsgCodec         ← 应用层协议（带 auth verify）
  └─ CodecCustom         ← 自定义扩展
```

---

## 发送数据流程

```
SendTo(msgShell, MsgHead, MsgBody):
  1. 查找连接 fd
  2. pSendBuff->Write(MsgHead.SerializeAsString())
  3. pSendBuff->Write(MsgBody.SerializeAsString())
  4. pSendBuff->WriteFD(fd, err)     ← 立即尝试 write
  5. pSendBuff->Compact(8192)
  6. if EAGAIN: RefreshEvent(EV_WRITE)  ← 注册写事件
     if error: DestroyConnect
```

**try-write-first**: 先尝试直接 write，失败才注册 EV_WRITE，避免不必要的 epoll_ctl。

---

## 压缩/加密管道

编码路径: `MsgBody.body → [Zip/Gzip] → [RC5/AES] → 组包 → CBuffer`

控制位（MsgHead.cmd）:
```
gc_uiGzipBit (0x10000000)  ← gzip 压缩
gc_uiZipBit  (0x20000000)  ← zip 压缩
gc_uiRc5Bit  (0x01000000)  ← RC5 加密
gc_uiAesBit  (0x02000000)  ← AES-128 加密
```

---

## CBuffer 缓冲区设计

```
       +-------------------+------------------+------------------+
       | readed bytes      |  readable bytes  |  writable bytes  |
       +-------------------+------------------+------------------+
       0      <=      readerIndex   <=   writerIndex    <=    capacity
```

| 操作 | 说明 |
|------|------|
| `Compact(8192)` | 释放已读空间；仍不足则 malloc 新 buffer |
| 扩容策略 | 容量不足时 ×2 扩容 |
| `BUFFER_MAX_READ` | 8192 字节，单次最多读取 |
| `DEFAULT_BUFFER_SIZE` | 32 字节初始容量 |
