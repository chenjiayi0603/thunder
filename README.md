# Thunder

分布式异步集群服务框架：中心节点注册发现、多节点 Worker、HTTP/自定义协议接入等。

## 能力概览

| 方向 | 说明 |
|------|------|
| **路由** | 中心节点支持注册、发现与路由 |
| **RPC / 异步** | 状态机、协程、远程过程调用（匿名函数等） |
| **IO** | 可自定义信号处理等 |
| **编解码** | 多种编解码器，可扩展 |

更细的编译与部署见仓库根目录 **[INSTALL.md](INSTALL.md)**。

## Hello 接入压测（wrk）

仓库提供 **wrk** 脚本，对默认 **ModuleHello** 的 HTTP 路径做压测（`POST` + `application/json`，body `{"option":"Echo"}`），与 **[deploy/tests/wrk_helloserver.lua](deploy/tests/wrk_helloserver.lua)**、**[deploy/tests/test_helloserver_wrk.sh](deploy/tests/test_helloserver_wrk.sh)** 一致。

**前置**：在 `deploy/` 下已启动 Hello（例如 **`./tests/test_helloserver.sh`**，见脚本注释）。端口、路径需与 **`deploy/Hello/conf/Hello.json`** 中 `access_host` / `access_port` 及 ModuleHello 的 `url_path` 一致（默认 `127.0.0.1:27006`，路径 `/hello/hello`）。

**执行**（在 `deploy/` 目录）：

```bash
./tests/test_helloserver_wrk.sh
```

可通过环境变量覆盖 host、端口、路径与 wrk 参数，例如：

```bash
HELLO_HOST=127.0.0.1 HELLO_PORT=27006 HELLO_PATH=/hello/hello \
  WRK_THREADS=4 WRK_CONNECTIONS=100 WRK_DURATION=10s \
  ./tests/test_helloserver_wrk.sh
```

**示例输出摘录**（单机一次跑数，完整日志见 **[deploy/tests/wrk_test_result.md](deploy/tests/wrk_test_result.md)**）：

- 环境：`4` 线程、`100` 连接、`10s`，URL `http://127.0.0.1:27006/hello/hello`
- 约 **54k req/s**，延迟 **P50 ≈ 2ms**（见该文件中的 Latency 分布）
- 若 wrk 中 **Non-2xx** 计数异常，请对照服务端实际 **HTTP 状态行** 与 ModuleHello 返回是否与 wrk 预期一致（详见 `wrk_helloserver.lua` 注释）

## 配置模板

Hello 等节点配置示例：**[deploy/Hello/conf/HelloTemplate.json](deploy/Hello/conf/HelloTemplate.json)**。

---

*历史说明：早期文档曾记录 siege 与另一套硬件下的 QPS 数据；当前以仓库内 **wrk** 脚本与 **`wrk_test_result.md`** 为准。*
