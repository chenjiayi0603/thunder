# Thunder 测试

## 统一入口

### 命令 + 环境要求

| 命令 | 运行环境 | 前置步骤 | 说明 |
|:---|:---|:---|:---|
| `./deploy.sh test unit` | 无 | 无 | C++ gtest + Python unit (~45s) |
| `./deploy.sh test e2e` | Docker + Docker Compose | 无 (自动 compose up) | 自动 compose up/down，清 etcd，端口预检 (~3min) |
| `./deploy.sh test` | Docker + Docker Compose | 无 (自动) | unit + e2e (~4min) |
| `./deploy.sh test smoke` | Docker Compose | `docker compose up -d` | 核心链路冒烟 (~5s) |
| `./deploy.sh test smoke --k8s` | K8s | 见下方「K8s 环境准备」 | 核心链路冒烟 K8s 模式 (~5s) |
| `./deploy.sh test regression` | Docker Compose 或 K8s | 自动检测 | 全量回归 (~5min) |
| `./deploy.sh test perf/bench` | Docker + Docker Compose | `docker compose up -d` | wrk 压测 (~1min) |

### 环境准备指令

**Docker Compose** — 启动全部服务：

```bash
cd docker
docker compose up -d
# 等待服务就绪 (~15s)
docker compose ps    # 确认全部 Running
```

**K8s** — 部署全部服务：

```bash
./deploy.sh image all          # 构建全部镜像
./deploy.sh push all           # 推送镜像到 local registry
./deploy.sh deploy             # kubectl apply 全部 K8s 资源
kubectl get pods -n thunder    # 确认全部 Running
```

K8s 回归测试 (额外独立脚本)：

```bash
bash k8s/regression-test.sh
```

### e2e 测试自动做的事

1. `docker compose down` — 清场
2. 清理 etcd bind-mount 数据（避免跨运行假故障）
3. 端口冲突预检（27006/27007/27010/27011/27443/27444）— 如被 K8s 占用则提示 `kubectl scale --replicas=0`
4. `docker compose build` — 构建所有服务镜像
5. `docker compose up -d` — 启动所有服务
6. `pytest tests/e2e/ -v --mode=local` — 运行 E2E 测试
7. `docker compose down` — 清理

### K8s 环境测试 (pytest, 需 `--mode=external`)

```bash
# admin-web API (13 用例)
E2E_ADMIN_HOST=192.168.3.61 pytest tests/e2e/test_admin_web.py -v --mode=external

# 灰度路由
E2E_ADMIN_HOST=192.168.3.61 pytest tests/e2e/test_canary_k8s.py -v --mode=external

# 稳定性压测 / 扩缩容 (bash)
bash tests/stability_test_k8s.sh
bash tests/test_k8s_scale.sh
```

## 测试分类

### 单元测试 (11 个) — `tests/unit/` — 零外部依赖

| 文件 | 内容 |
|:---|:---|
| `test_conhash.py` | 一致性哈希 |
| `test_etcd_config.py` | etcd 配置下发验证 |
| `test_etcd_registry.py` | etcd 注册中心 / lease / 路由传播 |
| `test_https_outbound.py` | HTTPS 出站代码路径 |
| `test_iobackend_behavior.py` | IoBackend 契约回归 |
| `test_json_parse.py` | JSON 解析边界 |
| `test_lua_hotreload.py` | Lua 热加载逻辑 (无网络) |
| `test_lua_script_api.py` | admin-web Lua 脚本 API |
| `test_node_id.py` | node_id 分配 / 槽位算法 |
| `test_token_verify.py` | GenKey/VerifyKey 逻辑 |
| `test_websocket_key.py` | WebSocket 握手密钥 |

### Docker Compose 端到端 — `tests/e2e/` — 默认模式

`conftest.py` 会话级 fixture 自动 `docker compose up/down`。加 `--mode=external` 切换到外部环境（K8s）。

| 文件 | 内容 |
|:---|:---|
| `test_http_hello.py` | HTTP Echo/CPU/Block |
| `test_https_hello.py` | HTTPS TLS |
| `test_ws_hello.py` | WebSocket |
| `test_interface_chain.py` | Interface→Logic 全链路 |
| `test_lua_module.py` | ModuleLua |
| `test_lua_hotreload_e2e.py` | Lua 热加载 (admin API → Worker) |
| `test_etcd_admin.py` | etcd Admin API |
| `test_etcd_stability.py` | etcd 全链路稳定性 (#115) |
| `test_etcd_watch.py` | etcd Watch 专项 |
| `test_canary_compose.py` | 灰度路由 (Docker Compose) |
| `test_https_outbound_e2e.py` | HTTPS 出站 (ModuleHello → 外部) |
| `test_stress.py` | 压力冒烟 |
| `test_wrk_smoke.py` | wrk 冒烟 |
| `test_etcd_regression.sh` | etcd 回归 (bash) |

### K8s 环境端到端

| 文件 | 模式 | 内容 |
|:---|:---|:---|
| `test_admin_web.py` | `--mode=external` | admin-web API (13 用例, NodePort 30090) |
| `test_canary_k8s.py` | `--mode=external` | K8s 灰度路由 |
| `k8s/regression-test.sh` | bash 直接运行 | 52 项 K8s 全链路回归 |
| `stability_test_k8s.sh` | bash | K8s 稳定性压测 |
| `test_k8s_scale.sh` | bash | 扩缩容 / Pod 自愈 / 滚动更新 |
| `test_smoke.sh` | bash | 冒烟测试 (docker-compose + k8s 双模式) |

### 进程级测试

| 文件 | 内容 |
|:---|:---|
| `test_graceful_restart.sh` | Worker 进程级优雅重启 |

### 性能测试 — `benchmark/`

| 文件 | 内容 |
|:---|:---|
| `run_bench.sh` | 全量三档横向对比 |
| `run_quick_bench.sh` | 快速冒烟 |
| `bench_asio_uring.sh` | asio_uring vs ev 对比 |
| `wrk_*.lua` | wrk 压测脚本 |

### C++ gtest — `code/test/` (20+ targets, ~250 cases)

| 目录 | 内容 |
|:---|:---|
| `connector/` | TcpCenterConnector 插件测试 |
| `coroutine/` | C++20 协程 |
| `codec/` | 编解码器 (proto/http/client) |
| `session/` | 会话管理 |

### 辅助脚本

| 文件 | 内容 |
|:---|:---|
| `run_all.sh` | → `../deploy.sh test` (向后兼容) |
| `check_env.sh` | 测试前环境预检 |
| `regression.sh` | 回归测试 (自动检测环境) |
| `save_status.sh` | 测试结果写入 `TEST_STATUS.md` |
| `logs.sh` | 日志收集 |
| `chaos_etcd.sh` | etcd 混沌测试 |
| `test_perf.sh` | wrk 压测入口 |
