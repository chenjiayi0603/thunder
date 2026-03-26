# HTTPS 编解码实现记录（CODEC_HTTPS）

本文档记录 `CODEC_HTTPS` 从后端编解码实现到部署拆分、证书生成、客户端请求示例、回归验证与排障的完整过程。

---

## 1. 改造目标

- 新增独立 HTTPS 编解码器，基于 OpenSSL 完成 TLS 握手和加解密。
- 复用现有 HTTP 业务处理链路，尽量不侵入 `HttpCodec` 语义。
- `Worker` 只做最小化路由与生命周期挂钩。
- 支持 Docker 一键起栈后直接验证 HTTP/WS/HTTPS 三条链路。

---

## 2. 关键代码改动总览

## 2.1 编解码与网络层

- `code/Util/src/util/StreamCodec.hpp`
  - 新增 `CODEC_HTTPS = 11`。
- `code/Net/include/codec/HttpsCodec.hpp`
- `code/Net/src/codec/HttpsCodec.hpp`
- `code/Net/src/codec/HttpsCodec.cpp`
  - 增加 TLS 状态管理、握手驱动、明密文缓冲转换、SSL/BIO 生命周期管理。
- `code/Net/include/labor/Worker.hpp`
- `code/Net/src/labor/Worker.cpp`
  - 增加 HTTPS 编解码分发、连接角色设置、连接销毁时 TLS 清理。
  - 增加 stale shell 防御（连接关闭后回包短路，减少错误风暴）。
- `code/Net/src/coro/StepCo20.cpp`
  - 协程错误/完成后短路 awaitable 发包，避免 timeout 后继续写失效 fd。

## 2.2 部署与构建

- `CMakeLists.txt`
- `code/Net/CMakeLists.txt`
- `code/Hello/CMakeLists.txt`
  - 将 Hello 运行产物拆分部署到 `HelloHttp/HelloWs/HelloHttps`。
  - 新增 `ModuleShake` 的构建和 `HelloWs/plugins/ModuleShake.so` 自动部署。
- `deploy/docker/docker-compose.yml`
  - 新增 `hello_https` 服务，`hello`/`hello_ws`/`hello_https` 分目录独立启动。
- `deploy/docker/dev_up_logs.sh`
  - 默认日志服务列表包含 `hello_ws` 和 `hello_https`。

## 2.3 新增/迁移目录与脚本

- `deploy/HelloHttp/*`
- `deploy/HelloWs/*`
- `deploy/HelloHttps/*`
  - 各自独立 `start.sh` / `stop.sh` / `conf` / `scripts` / `log` / `plugins`。
- `deploy/HelloHttps/scripts/gen_self_signed_https_cert.sh`
- `deploy/docker/test_helloserver_https_smoke.sh`

---

## 3. 后端设计与实现细节

### 3.1 HttpsCodec 分层

`HttpsCodec` 继承 `HttpCodec`，职责分层：

1. 应用层：HTTP 明文编解码复用 `HttpCodec`。
2. TLS 层：握手、加解密、会话状态由 `HttpsCodec` 维护。

### 3.2 连接级 TLS 状态（每 fd 独立）

`TlsConnState` 关键成员：

- `SSL_CTX* pCtx`
- `SSL* pSsl`
- `bool bServerSide`
- `bool bHandshakeDone`
- `util::CBuffer oPlainRecvBuff`
- `util::CBuffer oPendingPlainSend`

### 3.3 读路径（密文 -> 明文）

1. `Worker::IoRead` 收到网络密文进入 `pRecvBuff`
2. `HttpsCodec::FeedCipherToSsl` 写入 SSL 读 BIO
3. `DoHandshake` 推进握手（可多次 `WANT_READ/WANT_WRITE`）
4. `DrainSslToPlain` 从 `SSL_read` 得到明文
5. `HttpCodec::Decode` 解析 HTTP

### 3.4 写路径（明文 -> 密文）

1. 业务层生成 HTTP 明文（`HttpCodec::Encode`）
2. 握手未完成时先入 `oPendingPlainSend`
3. `SSL_write` 加密
4. `DrainOutboundCipher` 导出密文到 `pSendBuff`
5. `Worker::IoWrite` 发网卡

### 3.5 生命周期与防御

- 建连角色注入：
  - 入站：`server=true`
  - 出站：`server=false`
- 断连销毁时：
  - `HttpsCodec::RemoveConnection(fd)` 释放 `SSL/SSL_CTX`
- 防御改动：
  - `StepCo20` 在错误/完成后不再继续发异步请求
  - `Worker::SendToClient` 先检查 fd/seq 是否有效，失效则短路并降噪

---

## 4. TLS 握手说明（实现映射）

不论 TLS1.2 还是 TLS1.3，项目中统一由以下函数承载：

- `FeedCipherToSsl`：网络密文写入 RBIO
- `DoHandshake`：`SSL_do_handshake` 驱动状态机
- `DrainOutboundCipher`：从 WBIO 取出握手包/应用密文
- `DrainSslToPlain`：`SSL_read` 解密出应用明文

`bHandshakeDone=true` 后，连接进入稳定业务收发阶段。

---

## 5. 配置说明

HTTPS 典型配置：`deploy/HelloHttps/conf/HelloHttps.json`

关键字段：

- `access_codec: 11`（对应 `CODEC_HTTPS`）
- `access_port: 27443`
- `inner_port: 27444`
- `custom.https.server.cert_file`
- `custom.https.server.key_file`
- `custom.https.server.ca_file`
- `custom.https.server.verify_client`
- `custom.https.client.ca_file`
- `custom.https.client.verify_peer`

默认证书路径（相对 `HelloHttps` 目录）：

- `conf/certs/server.crt`
- `conf/certs/server.key`
- `conf/certs/ca.crt`

---

## 6. 证书生成

证书脚本（自签开发证书）：

```bash
./deploy/HelloHttps/scripts/gen_self_signed_https_cert.sh
```

生成产物：

- `deploy/HelloHttps/conf/certs/ca.key`
- `deploy/HelloHttps/conf/certs/ca.crt`
- `deploy/HelloHttps/conf/certs/server.key`
- `deploy/HelloHttps/conf/certs/server.crt`

脚本行为：

- 若 `ca.crt/server.crt/server.key` 均存在，则跳过生成。
- 否则自动生成 CA + 服务端证书（含 `127.0.0.1` 和 `localhost` SAN）。
- 私钥权限收敛为 `600`。

与冒烟脚本联动：

- `deploy/docker/test_helloserver_https_smoke.sh` 默认 `GENERATE_CERT=1`。
- 当证书缺失时，HTTPS smoke 会自动调用 `deploy/HelloHttps/scripts/gen_self_signed_https_cert.sh` 生成证书后继续测试。

---

## 7. 客户端请求示例

### 7.1 curl（推荐）

校验证书：

```bash
curl --cacert ./deploy/HelloHttps/conf/certs/ca.crt \
  -H 'Content-Type: application/json' \
  -X POST 'https://127.0.0.1:27443/hello/hello' \
  -d '{"option":"Echo"}'
```

跳过证书校验（仅调试）：

```bash
curl -k -H 'Content-Type: application/json' \
  -X POST 'https://127.0.0.1:27443/hello/hello' \
  -d '{"option":"Echo"}'
```

### 7.2 Python（requests）

```python
import requests

url = "https://127.0.0.1:27443/hello/hello"
payload = {"option": "Echo"}
r = requests.post(url, json=payload, verify="./deploy/HelloHttps/conf/certs/ca.crt", timeout=10)
print(r.status_code, r.text)
```

### 7.3 Go（net/http）

```go
package main

import (
  "crypto/tls"
  "crypto/x509"
  "fmt"
  "io"
  "net/http"
  "os"
  "strings"
)

func main() {
  ca, _ := os.ReadFile("./deploy/HelloHttps/conf/certs/ca.crt")
  pool := x509.NewCertPool()
  pool.AppendCertsFromPEM(ca)

  cli := &http.Client{
    Transport: &http.Transport{
      TLSClientConfig: &tls.Config{RootCAs: pool},
    },
  }

  resp, err := cli.Post("https://127.0.0.1:27443/hello/hello", "application/json", strings.NewReader(`{"option":"Echo"}`))
  if err != nil { panic(err) }
  defer resp.Body.Close()
  b, _ := io.ReadAll(resp.Body)
  fmt.Println(resp.StatusCode, string(b))
}
```

---

## 8. 部署结构（拆分后）

为避免同目录多入口互相影响，已拆分三套目录：

- `deploy/HelloHttp`：HTTP 节点
- `deploy/HelloWs`：WebSocket 节点
- `deploy/HelloHttps`：HTTPS 节点

每套目录自带：

- `bin/`
- `conf/`
- `plugins/`
- `scripts/`
- `log/`
- `start.sh` / `stop.sh`

Docker 服务映射：

- `hello` -> `HelloHttp`
- `hello_ws` -> `HelloWs`
- `hello_https` -> `HelloHttps`

---

## 9. 自动化验证

### 9.1 起栈与日志

```bash
./deploy/docker/dev_up_logs.sh restart
```

### 9.2 三协议 smoke

```bash
./deploy/docker/test_helloserver_smoke.sh
./deploy/docker/test_helloserver_ws_smoke.sh
./deploy/docker/test_helloserver_https_smoke.sh
```

HTTPS smoke 常用环境变量：

- `START_HTTPS_NODE=0|1`
- `REQUIRE_PORTS=0|1`
- `INSECURE_TLS=0|1`
- `HELLO_HTTPS_HOST`
- `HELLO_HTTPS_PORT`（默认 `27443`）

---

## 10. 常见问题与排障

### 10.1 HTTP 000 / connection refused

现象：

- `curl: (7) Failed to connect ...`

排查：

```bash
./deploy/docker/dev_up_logs.sh status
docker compose -f deploy/docker/docker-compose.yml logs --tail 120 hello
```

### 10.2 容器 Exited (1) 且提示 `start.sh.log: No such file or directory`

原因：

- 对应目录缺 `log/`，脚本写日志失败且 `set -e` 退出。

修复：

- 确保 `deploy/HelloHttp/log`、`deploy/HelloWs/log`、`deploy/HelloHttps/log` 存在。

### 10.3 WS smoke 失败提示缺少 `ModuleShake.so`

原因：

- `ModuleShake` 未构建或未部署到 `HelloWs/plugins`。

修复：

```bash
cmake --build build -j1
cmake --install build
ls deploy/HelloWs/plugins
```

### 10.4 运行时符号不匹配

现象：

- `undefined symbol ... HttpsCodec::EncodeToConnection`

修复：

```bash
cmake --build build -j1
cmake --install build
./deploy/docker/dev_up_logs.sh restart
```

---

## 11. 兼容性结论

- `CODEC_HTTPS` 为独立路径，不影响原有 HTTP/PB/Private/WS 编解码语义。
- `Worker` 改动限定在 codec 分发、连接清理和安全短路。
- 目录拆分后，HTTP/WS/HTTPS 服务启动边界清晰，避免同进程目录扫描导致的重复拉起和端口冲突。

---

## 12. 后续可选增强

- 支持 TLS 版本、密码套件白名单配置化。
- 增加双向证书校验（mTLS）回归用例。
- 将 HTTPS smoke 纳入统一 `test_all_docker_smoke.sh` 强制门禁。

