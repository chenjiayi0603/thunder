# Thunder HTTPS 编解码器：实现原理与运维指南

> 合并自 `https_codec_implementation_record.md` 与 `Thunder_HTTPS编解码器实现与原理分析.md`

---

## 一、继承体系与类型枚举

### 1.1 四层继承架构

```
CStreamCodec          (枚举定义: E_CODEC_TYPE)
  └─ ThunderCodec     (基类: 压缩/加密/CBuffer 封装)
       └─ HttpCodec   (HTTP 明文: http_parser 集成)
            └─ HttpsCodec  (HTTPS = HTTP + TLS)
```

每层只关注自身职责，下层对上层透明：
- **HttpCodec** 可独立于 TLS 运行，承载 HTTP 请求/响应解析与组装
- **HttpsCodec** 只增加 TLS 加解密层，HTTP 逻辑完全复用基类

### 1.2 E_CODEC_TYPE 枚举

```cpp
// code/Util/src/util/StreamCodec.hpp
enum E_CODEC_TYPE {
    CODEC_UNKNOW             = 0,
    CODEC_PB_INTERNAL        = 2,
    CODEC_HTTP               = 3,
    CODEC_PRIVATE            = 4,
    CODEC_WEBSOCKET_EX_JS    = 5,
    CODEC_WEBSOCKET_EX_PB    = 6,
    CODEC_TLV                = 7,
    CODEC_TEST               = 8,
    CODEC_APP                = 9,
    CODEC_WEBSOCKET_EX_PB_APP = 10,
    CODEC_HTTPS              = 11,   // ← 独立路径，非 HTTP 扩展
    CODEC_WSS                = 12,
};
```

### 1.3 E_CODEC_STATUS 状态机

```cpp
enum E_CODEC_STATUS {
    CODEC_STATUS_OK    = 0,  // 成功
    CODEC_STATUS_ERR   = 1,  // 错误（调用方关闭连接）
    CODEC_STATUS_PAUSE = 2,  // 数据不完整或握手未完成，等待下一轮 IO
};
```

**PAUSE 的语义**：非错误，告诉 IO 调度器继续监听该连接；握手 WANT_READ/WANT_WRITE 均返回此值。

---

## 二、核心数据结构

### 2.1 TlsConnState（每连接独立）

```cpp
// code/Net/src/codec/HttpsCodec.hpp
struct TlsConnState {
    SSL_CTX* pCtx          = nullptr;  // SSL 上下文（可共享配置）
    SSL*     pSsl          = nullptr;  // SSL 连接对象（每连接独立）
    bool     bServerSide   = false;    // true=服务端被动握手，false=客户端主动握手
    bool     bHandshakeDone = false;   // 握手完成标志

    util::CBuffer oPlainRecvBuff;      // SSL_read 解出的明文缓冲
    util::CBuffer oPendingPlainSend;   // 握手期间暂存的待发明文
};
```

生命周期：
1. `EnsureState()` — 首次 Decode 时创建，SSL_CTX_new → SSL_new → BIO_new
2. 运行中 — Decode/Encode 循环驱动
3. `RemoveConnection(fd)` — 连接关闭时 SSL_shutdown → SSL_free → SSL_CTX_free

### 2.2 HttpsConfig（HTTPS 服务配置）

```cpp
struct HttpsConfig {
    std::string strServerCertFile;    // PEM 格式服务端证书
    std::string strServerKeyFile;     // PEM 格式服务端私钥
    std::string strServerCaFile;      // CA 证书（双向认证验证客户端）
    bool bServerVerifyClient = false; // 是否要求客户端证书

    std::string strClientCaFile;      // 客户端 CA 证书（验证服务端）
    bool bClientVerifyPeer   = false; // 是否验证服务端证书
};
```

配置组合：

| 场景 | 服务端 | 客户端 |
|------|--------|--------|
| 单向 TLS（标准 HTTPS） | cert + key | 可选 CA 验证 |
| 双向 mTLS | cert + key + CA + verify | CA + verify |

### 2.3 BIO 内存缓冲机制

OpenSSL 使用内存型 BIO（`BIO_s_mem()`）与框架现有 IO 系统解耦：

```
网络密文 (pConn->pRecvBuff)
        │
        ▼ BIO_write
     RBIO (SSL 读缓冲)
        │
   [TLS 引擎]
        │
     WBIO (SSL 写缓冲)
        │ BIO_read
        ▼
密文输出 (pConn->pSendBuff)
```

密文的读写完全由框架控制，OpenSSL 内部状态机通过 RBIO/WBIO 与外界交互。

---

## 三、读路径：密文 → 明文 → HTTP

```
Client TLS密文
    │
    ▼ FeedCipherToSsl()
  RBIO ── BIO_write 把 pRecvBuff 内容送入 SSL 读端
    │
    ▼ DoHandshake()
  握手状态机（可多轮 WANT_READ/WANT_WRITE）
    │
    ▼ DrainSslToPlain()
  SSL_read() 循环取明文 → oPlainRecvBuff
    │
    ▼ HttpCodec::Decode()
  http_parser 解析 → MsgHead + MsgBody
```

### FeedCipherToSsl

```cpp
int n = BIO_write(SSL_get_rbio(pState->pSsl),
                  pConn->pRecvBuff->GetRawReadBuffer(), iLen);
pConn->pRecvBuff->AdvanceReadIndex(n);  // 必须消费已写入数据
```

### DoHandshake（握手状态机）

```cpp
int ret = SSL_do_handshake(pState->pSsl);
DrainOutboundCipher(pState, pOutBuff);  // 无论成败，先刷出握手密文

if (ret == 1) {
    pState->bHandshakeDone = true;
    return CODEC_STATUS_OK;
}
int err = SSL_get_error(pState->pSsl, ret);
if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return CODEC_STATUS_PAUSE;   // 非错误，等待下一轮 IO
}
return CODEC_STATUS_ERR;
```

握手流程（TLS 1.3 简化）：
```
Client                      Server
  │── ClientHello ─────────►│  WANT_WRITE → 发握手包
  │◄─ ServerHello+Cert ─────│  WANT_READ → 等待接收
  │◄─ Finished ─────────────│  WANT_READ
  │── Finished ─────────────►│  握手完成 bHandshakeDone=true
  │◄══════ 数据传输 ══════════│
```

TLS 版本透明处理：`TLS_server_method()` / `TLS_client_method()` 自动启用支持的最高版本（1.2 或 1.3），应用层无感。

---

## 四、写路径：HTTP → 明文 → 密文

```
MsgHead + MsgBody
    │
    ▼ HttpCodec::Encode()
  oPlainBuff（HTTP 明文）
    │
    ▼ EncryptPlain()
  ├── 握手未完成 → oPendingPlainSend 暂存，返回 PAUSE
  └── 握手完成  → SSL_write() 加密
    │
    ▼ DrainOutboundCipher()
  BIO_read(WBIO) 取出密文 → pConn->pSendBuff
    │
    ▼ Worker::IoWrite()
  发网卡
```

### 握手期间明文暂存

```
T1: EncodeToConnection 调用
T2: bHandshakeDone=false → 明文写入 oPendingPlainSend
T3: 返回 PAUSE（不是错误）
... 握手进行中 ...
T4: Decode 检测到握手完成
T5: 将 oPendingPlainSend 中数据写入 SSL_write，清空缓冲
```

---

## 五、连接生命周期与角色注入

### 角色注入（Worker 层）

```cpp
// 入站连接 = 服务端
void Worker::OnConnectionAccepted(int iFd) {
    m_pCodec->SetConnectionRole(iFd, true);   // bServerSide=true → TLS_server_method
}
// 出站连接 = 客户端
void Worker::OnConnectionOpened(int iFd) {
    m_pCodec->SetConnectionRole(iFd, false);  // bServerSide=false → TLS_client_method
}
```

延迟创建：SSL 对象在首次 Decode 时才通过 `EnsureState()` 创建，角色信息通过 `m_mapConnRole` 缓存传递。

### Stale Shell 防御

连接关闭后，之前请求的回包可能仍在 IO 队列。Thunder 的防御：
- `Worker::SendToClient` 先检查 fd/seq 有效性，失效则短路降噪
- `StepCo20` 协程在错误/完成后不继续发送异步请求

---

## 六、与 IO 后端的协作

Thunder 支持多种 IO 后端（Epoll、io_uring、Asio+io_uring）。TLS 写操作对后端有特殊要求：

```
Epoll/io_uring 可读事件
        │
        ▼ Decode()
  FeedCipherToSsl → DoHandshake → DrainSslToPlain → HttpCodec::Decode
        │
        ▼ PAUSE → 继续监听（等待握手完成）

可写事件
        │
        ▼ DrainOutboundCipher → Send
  消费 WBIO 密文发送到网络
```

UringIoBackend 的 TLS 写操作采用同步策略（`::write` 同步系统调用），原因是 TLS 写可能需要多轮 SSL_write，异步提交会导致状态不一致。

---

## 七、配置说明

### 7.1 服务配置文件

`deploy/HelloHttps/conf/HelloHttps.json` 关键字段：

| 字段 | 值 | 说明 |
|------|----|------|
| `access_codec` | `11` | 对应 `CODEC_HTTPS` |
| `access_port` | `27443` | 对外 TLS 端口 |
| `inner_port` | `27444` | 内部通信端口 |
| `custom.https.server.cert_file` | `conf/certs/server.crt` | 服务端证书 |
| `custom.https.server.key_file` | `conf/certs/server.key` | 服务端私钥 |
| `custom.https.server.ca_file` | `conf/certs/ca.crt` | CA 证书 |
| `custom.https.server.verify_client` | `false` | 是否要求客户端证书 |

### 7.2 证书生成（自签开发证书）

```bash
./deploy/HelloHttps/scripts/gen_self_signed_https_cert.sh
```

脚本行为：
- 若 `ca.crt/server.crt/server.key` 均存在则跳过
- 自动生成 CA + 服务端证书（含 `127.0.0.1` 和 `localhost` SAN）
- 私钥权限收敛为 `600`

生成产物（相对 `deploy/HelloHttps/`）：
```
conf/certs/
  ca.key / ca.crt
  server.key / server.crt
```

手动生成（含双向认证客户端证书）：

```bash
# CA
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 365 -key ca.key -out ca.crt \
    -subj "/CN=Thunder CA"

# 服务端
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr -subj "/CN=localhost"
echo "subjectAltName=DNS:localhost,IP:127.0.0.1" > ext.cnf
openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key \
    -extfile ext.cnf -CAcreateserial -out server.crt

# 客户端（mTLS）
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr -subj "/CN=Client"
openssl x509 -req -days 365 -in client.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out client.crt
```

---

## 八、部署结构

三套目录独立，避免同进程目录扫描导致端口冲突：

```
deploy/
  HelloHttp/     # HTTP 节点 (CODEC_HTTP=3, port 27006)
  HelloWs/       # WebSocket 节点 (port 27007)
  HelloHttps/    # HTTPS 节点 (CODEC_HTTPS=11, port 27443)
    bin/
    conf/certs/  # 证书目录
    plugins/
    log/
    start.sh / stop.sh
```

Docker 服务映射：
- `hello` → `HelloHttp`
- `hello_ws` → `HelloWs`
- `hello_https` → `HelloHttps`

---

## 九、客户端请求示例

### curl

```bash
# 校验证书（推荐）
curl --cacert ./deploy/HelloHttps/conf/certs/ca.crt \
  -H 'Content-Type: application/json' \
  -X POST 'https://127.0.0.1:27443/hello/hello' \
  -d '{"option":"Echo"}'

# 跳过证书校验（仅调试）
curl -k -X POST 'https://127.0.0.1:27443/hello/hello' \
  -d '{"option":"Echo"}'

# mTLS（双向认证）
curl --cert client.crt --key client.key --cacert ca.crt \
  https://127.0.0.1:27443/hello/hello
```

### Python（requests）

```python
import requests
r = requests.post(
    "https://127.0.0.1:27443/hello/hello",
    json={"option": "Echo"},
    verify="./deploy/HelloHttps/conf/certs/ca.crt",
    timeout=10
)
print(r.status_code, r.text)
```

可选环境变量：`HELLO_HTTPS_HOST`、`HELLO_HTTPS_PORT`（默认 27443）、`HELLO_HTTPS_CA`、`HELLO_HTTPS_INSECURE=1`

### Go

```go
ca, _ := os.ReadFile("./deploy/HelloHttps/conf/certs/ca.crt")
pool := x509.NewCertPool()
pool.AppendCertsFromPEM(ca)
cli := &http.Client{
    Transport: &http.Transport{
        TLSClientConfig: &tls.Config{RootCAs: pool},
    },
}
resp, _ := cli.Post("https://127.0.0.1:27443/hello/hello",
    "application/json", strings.NewReader(`{"option":"Echo"}`))
```

---

## 十、自动化验证

```bash
# 起栈 + 查日志
./deploy/docker/dev_up_logs.sh restart

# 全量 E2E（本地模式：自动 compose up/down）
./deploy.sh test e2e

# 外部模式（连接现有环境，不管 compose 生命周期）
MODE=external ./tests/run_all.sh e2e
```

HTTPS smoke 常用环境变量：
- `START_HTTPS_NODE=0|1`
- `REQUIRE_PORTS=0|1`
- `INSECURE_TLS=0|1`

---

## 十一、常见问题排障

| 现象 | 原因 | 处理 |
|------|------|------|
| `curl: (7) Failed to connect` | 服务未启动或端口未监听 | `docker compose logs hello_https` |
| 容器 `Exited (1)` / `start.sh.log: No such file` | 缺少 `log/` 目录 | 确保 `deploy/HelloHttps/log/` 存在 |
| `undefined symbol HttpsCodec::EncodeToConnection` | 二进制未更新 | `cmake --build build && cmake --install build && restart` |
| 握手超时 | 网络/防火墙 | 增加超时配置，检查 iptables |
| 证书验证失败 | 过期或不受信 | 更新证书，添加 CA |
| 双向认证失败 | 客户端证书未由服务端 CA 签发 | 检查 client.crt 签发链 |
| `WANT_READ/WANT_WRITE` 过多 | IO 线程负载高 | 检查 Epoll/uring 线程 |

---

## 十二、兼容性与后续

**兼容性**：`CODEC_HTTPS` 为独立路径，不影响原有 HTTP/WS/WSS 编解码语义；Worker 改动限定在 codec 分发、连接清理和安全短路。

**后续可选增强**：
- TLS 版本、密码套件白名单配置化
- mTLS 双向认证的完整回归用例
- HTTPS 用例纳入 E2E 强制门禁

---

## 附录：关键代码索引

| 文件 | 路径 | 说明 |
|------|------|------|
| `StreamCodec.hpp` | `code/Util/src/util/` | `E_CODEC_TYPE` 枚举 |
| `ThunderCodec.hpp/cpp` | `code/Net/src/codec/` | 基类、`E_CODEC_STATUS` |
| `HttpCodec.hpp/cpp` | `code/Net/src/codec/` | HTTP 明文编解码 |
| `HttpsCodec.hpp/cpp` | `code/Net/src/codec/` | HTTPS 实现 |
| `HttpsCodec.hpp` (include) | `code/Net/include/codec/` | 对外头文件 |

| 函数 | 作用 |
|------|------|
| `EnsureState()` | 创建/获取 `TlsConnState` |
| `FeedCipherToSsl()` | 密文注入 RBIO |
| `DoHandshake()` | 握手状态机驱动 |
| `DrainSslToPlain()` | `SSL_read` 消费明文 |
| `EncryptPlain()` | `SSL_write` 加密明文 |
| `DrainOutboundCipher()` | 从 WBIO 刷出密文 |
| `RemoveConnection()` | 连接清理，释放 SSL/SSL_CTX |
