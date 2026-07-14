# Thunder 快速上手

> 更新: 2026-07-14 | 🟢 = Docker Compose | 🔵 = K8s

---

## 一、构建

> 🟢🔵 通用

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1
cmake --build build -j1
cmake --install build
```

> ⚠️ 必须 `-j1`，多线程编译会因磁盘 IO 瓶颈卡死。

---

## 二、启动

### 🟢 Docker Compose

```bash
./deploy.sh up          # 启动集群（3-node etcd + MySQL + Redis + 全部服务）
./deploy.sh status      # 查看容器状态 + 监听端口
./deploy.sh restart     # 重启
./deploy.sh down        # 停止并清理
```

等待约 15 秒，所有服务 healthy 后测试。

### 🔵 Kubernetes

```bash
# 一键发布：编译 → 镜像 → 导入 containerd → 部署 → 回归
./deploy.sh release k8s

# 或分步：
./deploy.sh build                          # cmake 编译
./deploy.sh image logic interface logic-v2 hello http https ws wss
./deploy.sh deploy                         # 导入 containerd + kubectl apply
bash k8s/regression-test.sh                # 回归测试 (19 项)
```

**手动导入镜像** (`deploy.sh deploy` 需要 sudo 密码):

```bash
docker save thunder-logic:latest | sudo ctr -n k8s.io image import -
```

---

## 三、测试

### 🔵 K8s 回归测试

```bash
bash k8s/regression-test.sh
# 通过: 19  失败: 0  跳过: 0
```

覆盖：CoreDNS / 5 网关 Pod / 插件隔离 / DNS 解析 / 服务直连

### 🟢 Docker Compose 冒烟测试

```bash
./tests/test_smoke.sh
```

覆盖 HTTP / HTTPS / WebSocket / Interface→Logic 全链路，9 项全绿即通过。

### 🟢🔵 自动化测试

```bash
./deploy.sh test unit       # C++ + Python 单元，零外部依赖，~45s
./deploy.sh test e2e        # Docker E2E 集成，~3min
./deploy.sh test            # 全部
```

### 🔵 K8s 快速验证

```bash
# HTTP Echo
curl -s -X POST http://192.168.3.61:27006/hello/hello \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":5}'
# → {"code":0,"msg":"ok","size":5,"data":"XXXXX"}

# HTTPS
curl -sk -X POST https://192.168.3.61:27443/hello/hello \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":3}'
# → HTTP 200

# Interface → Logic
curl -s http://192.168.3.61:27008/Interface/gentoken
# → {"code":400,"msg":"ok"}
```

---

## 四、灰度发布 (Canary)

> 🔵 K8s（🟢 Compose 将 etcd 地址改为 `127.0.0.1:12379` 即可）

```bash
# 🔵 端口转发 etcd 到本地
kubectl port-forward -n thunder pod/thunder-etcd-0 12379:2379 &

# 🟢 Compose 直接连（etcd 在宿主机 12379）
# export ETCD_ENDPOINT=127.0.0.1:12379

export ETCD_ENDPOINT=127.0.0.1:12379
```

### 查看权重

```bash
python3 tools/canary.py LOGIC
# →  LOGIC: 无灰度配置（使用默认一致性哈希路由）
```

### 灰度 v2 占 30%

```bash
python3 tools/canary.py LOGIC canary v2 30
# → ✅ LOGIC: {"v2": 30, "v1": 70}
#     v1:   70 ( 70.0%)  ██████████████░░░░░░
#     v2:   30 ( 30.0%)  ██████░░░░░░░░░░░░░░
```

### 全量切换 v2

```bash
python3 tools/canary.py LOGIC full v2
# → ✅ LOGIC 全量切换 → v2
```

### 回滚

```bash
python3 tools/canary.py LOGIC rollback
# → ✅ LOGIC 已回滚 → v1=100%
```

### 清除灰度

```bash
python3 tools/canary.py LOGIC reset
# → ✅ LOGIC 灰度配置已清除，恢复一致性哈希路由
```

**全链路**: `canary.py → etcd /thunder/canary/LOGIC/weights → Manager Watch → Worker 加权随机路由`

🔵 验证 Logic 已收到: `kubectl logs -n thunder deploy/thunder-logic | grep DoCanarySnapshot`

---

## 五、热更新

### Lua 热更新

#### 🔵 K8s — 直接 etcd（跳过 Admin，适合脚本/CI）

```bash
ETCD_POD=$(kubectl get pods -n thunder -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}')

# 1. 写新 Lua 脚本（version 从 12 升到 99）
kubectl exec -n thunder "$ETCD_POD" -- etcdctl --endpoints=http://127.0.0.1:2379 \
  put /thunder/config/module/HELLO_HTTP \
  '{"module":[{"url_path":"/hello/lua_echo","so_path":"plugins/HelloHttp_ModuleLua.so","entrance_symbol":"create","load":true,"version":99,"script_content":"function handle_request(msg)\n  SendToClientFast('"'"'{\"code\":0,\"msg\":\"HOTRELOAD_V99\"}'"'"')\n  return true\nend"}]}'

# 2. 验证
curl -s -X POST http://192.168.3.61:27006/hello/lua_echo -d 'test'
# → {"code":0,"msg":"HOTRELOAD_V99"}

# 3. 日志确认
kubectl logs -n thunder deploy/thunder-hello --tail=10 | grep -E "Unload|Load|ModuleLua"
# → UnloadSoAndDeleteModule → LoadSoAndGetModule → ModuleLua::Init
```

#### 🟢 Docker Compose — Admin API

```bash
curl -X POST http://127.0.0.1:8090/api/lua-scripts \
  -H "Content-Type: application/json" \
  -d '{"node_type":"HELLO_HTTP","name":"echo.lua","url_path":"/hello/lua_echo","content":"function handle_request(msg)\n  SendToClientFast('"'"'{\"code\":0,\"msg\":\"HOTRELOAD\"}'"'"')\n  return true\nend"}'

curl -X POST http://127.0.0.1:8090/api/sync-config
```

**全链路**: `Admin → etcd /thunder/config/module/{TYPE} → Manager Watch → CMD_REQ_RELOAD_LUA → Worker dlclose/dlopen → ModuleLua::Init → 新脚本执行`

### SO 热更新

#### 🔵 K8s — 重建镜像 + 滚动更新

```bash
./deploy.sh build
./deploy.sh image hello
docker save thunder-hello-http:latest | sudo ctr -n k8s.io image import -
kubectl rollout restart deploy/thunder-hello -n thunder
kubectl rollout status deploy/thunder-hello -n thunder
```

#### 🟢 Docker Compose — Admin SO 上传

```bash
curl -X PUT http://127.0.0.1:8090/plugins/HelloHttp/HelloHttp_ModuleHello.so \
  --data-binary @build/lib/HelloHttp_ModuleHello.so
# → etcd version++ → Worker GracefulRestart → dlopen 新 .so
```

> 🔵 SO 已烘焙在 Docker 镜像（`/app/plugins/`），K8s 通过重建镜像部署。
> 🟢 Compose 用 volume 挂载，直接 PUT 文件即可，无需重建镜像。

---

## 六、端口速查

| 服务 | 协议 | 端口 | 🟢 Compose | 🔵 K8s |
|------|------|------|:--:|:--:|
| HelloHttp | HTTP | 27006 | `127.0.0.1` | `192.168.3.61` |
| HelloHttps | HTTPS | 27443 | `127.0.0.1` | `192.168.3.61` |
| HelloWs | WebSocket | 27010 | `127.0.0.1` | `192.168.3.61` |
| HelloWss | WSS | 27012 | `127.0.0.1` | `192.168.3.61` |
| Interface | HTTP | 27008 | `127.0.0.1` | `192.168.3.61` |
| Logic v1 | S2S | 16068 | `127.0.0.1` | ClusterIP |
| Logic v2 | S2S | 16069 | `127.0.0.1` | ClusterIP |
| etcd | gRPC | 12379/2379 | `127.0.0.1:12379` | svc:2379 |
| Admin Web | HTTP | 8090 | `127.0.0.1:8090` | ClusterIP |

---

## 七、清理

```bash
./deploy.sh clean               # 🟢🔵 清理 build/ + Docker

# 🔵 K8s 完全清理
kubectl delete ns thunder
```

---

## 附录：关键设计文档

| 主题 | 文档 |
|------|------|
| Canary 灰度路由 | `docs/architecture/34-k8s-canary-routing.md` |
| entrypoint + Compose | `docs/architecture/37-entrypoint-and-docker-compose-canary.md` |
| K8s 运维 | `k8s/OPERATIONS.md` |
| SO 热更新 via etcd | `docs/architecture/15-so-module-hot-reload-via-etcd.md` |
| Lua 模块 | `docs/architecture/21-lua-send-to-node-type.md` |
| Manager/Worker IPC | `docs/architecture/11-manager-worker-ipc.md` |
