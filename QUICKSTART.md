# Thunder 快速上手

> 更新: 2026-07-14 | 🟢 = Docker Compose | 🔵 = K8s

---

## 一、构建

> 🟢🔵 通用

```bash
# 首次：拉取三方库源码
git submodule update --init --recursive

# 生成 Makefile（只需执行一次，之后重编不需要）
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 编译三方库（curl/grpc/protobuf/openssl 等，仅首次 ~20min）
cmake --build build --target thirdparty_deploy -j1

# 编译 Thunder 主工程 + 安装二进制到 deploy/*/bin/
cmake --build build -j1
cmake --install build
```

> ⚠️ 必须 `-j1`，多线程编译会因磁盘 IO 瓶颈卡死。

---

## 二、启动

### 🟢 Docker Compose

```bash
./deploy.sh up          # 启动全部服务（etcd×3 + MySQL + Redis + 7 个 Thunder 节点）
./deploy.sh status      # 查看容器状态 + 监听端口
./deploy.sh restart     # 重启所有容器
./deploy.sh down        # 停止并清理
```

等待约 15 秒，所有服务 healthy 后测试。

### 🔵 Kubernetes

```bash
# 一键发布（编译 + 镜像 + 部署 + 回归）
./deploy.sh release k8s

# 或分步执行：
./deploy.sh build                          # cmake 编译 → 产出二进制到 deploy/*/bin/
./deploy.sh image logic interface logic-v2 hello http https ws wss
# ↑ 为每个服务构建 Docker 镜像（COPY deploy/XXX/ → /app/）
./deploy.sh deploy                         # 导入镜像到 containerd + kubectl apply YAML
bash k8s/regression-test.sh                # 回归测试（19 项）
```

**手动导入镜像** (`deploy.sh deploy` 需要 sudo 密码):

```bash
# 将 Docker 镜像导出为 tar，导入到 k3s/containerd 运行时
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
./deploy.sh test unit       # C++ gtest + Python pytest，零外部依赖，~45s
./deploy.sh test e2e        # Docker E2E 集成测试（需 Docker），~3min
./deploy.sh test            # unit + e2e 全跑
```

### 🔵 K8s 快速验证

```bash
# HelloHttp Echo 接口
curl -s -X POST http://192.168.3.61:27006/hello/hello \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":5}'
# → {"code":0,"msg":"ok","size":5,"data":"XXXXX"}

# HelloHttps 加密 Echo
curl -sk -X POST https://192.168.3.61:27443/hello/hello \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":3}'
# → HTTP 200

# Interface → Logic GenKey 网关
curl -s http://192.168.3.61:27008/Interface/gentoken
# → {"code":400,"msg":"ok"}  (400 = 正常响应，需带完整参数)
```

---

## 四、灰度发布 (Canary)

> 🟢🔵 通用（🟢 Compose 直接连 `127.0.0.1:12379`，🔵 K8s 需先 port-forward）

```bash
# 🔵 K8s: 端口转发 etcd 到本地
kubectl port-forward -n thunder pod/thunder-etcd-0 12379:2379 &

# 🟢 Compose: etcd 已在宿主机 12379 端口
export ETCD_ENDPOINT=127.0.0.1:12379
```

### 查看当前权重

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
# → ✅ LOGIC 全量切换 → v2（v1 权重归零，保留版本记录）
```

### 回滚到稳定版本

```bash
python3 tools/canary.py LOGIC rollback
# → ✅ LOGIC 已回滚 → v1=100%
```

### 清除灰度（恢复一致性哈希）

```bash
python3 tools/canary.py LOGIC reset
# → ✅ LOGIC 灰度配置已清除，恢复一致性哈希路由
```

**全链路**: `canary.py` → 写 etcd `/thunder/canary/LOGIC/weights` → Manager Watch → Worker 加权随机路由

🔵 验证 Logic 已收到权重: `kubectl logs -n thunder deploy/thunder-logic | grep DoCanarySnapshot`

---

## 五、热更新

### Lua 热更新

#### 🔵 K8s — 直接写 etcd（跳过 Admin，适合脚本/CI）

```bash
ETCD_POD=$(kubectl get pods -n thunder -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}')

# 1. 写新 Lua 脚本到 etcd（version 从 12 升到 99，script_content 含新逻辑）
kubectl exec -n thunder "$ETCD_POD" -- etcdctl --endpoints=http://127.0.0.1:2379 \
  put /thunder/config/module/HELLO_HTTP \
  '{"module":[{"url_path":"/hello/lua_echo","so_path":"plugins/HelloHttp_ModuleLua.so","entrance_symbol":"create","load":true,"version":99,"script_content":"function handle_request(msg)\n  SendToClientFast('"'"'{\"code\":0,\"msg\":\"HOTRELOAD_V99\"}'"'"')\n  return true\nend"}]}'
# → OK（Manager Watch 检测到 version 12→99，触发 Worker 热重载）

# 2. 验证新逻辑已生效
curl -s -X POST http://192.168.3.61:27006/hello/lua_echo -d 'test'
# → {"code":0,"msg":"HOTRELOAD_V99"}

# 3. Worker 日志确认全链路
kubectl logs -n thunder deploy/thunder-hello --tail=10 | grep -E "Unload|Load|ModuleLua"
# → UnloadSoAndDeleteModule() → LoadSoAndGetModule() → ModuleLua::Init()
# → script_content_len=101   (101 字节新脚本已加载)
```

#### 🟢 Docker Compose — Admin API

```bash
# 通过 Admin 上传 Lua 脚本
curl -X POST http://127.0.0.1:8090/api/lua-scripts \
  -H "Content-Type: application/json" \
  -d '{"node_type":"HELLO_HTTP","name":"echo.lua","url_path":"/hello/lua_echo","content":"function handle_request(msg)\n  SendToClientFast('"'"'{\"code\":0,\"msg\":\"HOTRELOAD\"}'"'"')\n  return true\nend"}'

# 同步配置到 etcd（触发 Manager Watch → Worker 重载）
curl -X POST http://127.0.0.1:8090/api/sync-config
```

**全链路**: `Admin/etcd` → etcd key `/thunder/config/module/{TYPE}` → Manager Watch 检测 version 变化 → `CMD_REQ_RELOAD_LUA` → Worker `dlclose/dlopen` SO → `ModuleLua::Init()` 执行新脚本

### SO 热更新

#### 🔵 K8s — 重建镜像 + 滚动更新

```bash
# SO 模块代码变更后，重新编译 + 构建镜像
./deploy.sh build                          # cmake 编译新 .so
./deploy.sh image hello                    # 构建含新 .so 的 Docker 镜像
docker save thunder-hello-http:latest | sudo ctr -n k8s.io image import -
                                           # 导入到 k3s containerd 运行时
kubectl rollout restart deploy/thunder-hello -n thunder
                                           # 滚动更新 Pod（新 Pod 启动含新 .so）
kubectl rollout status deploy/thunder-hello -n thunder
                                           # 等待更新完成
```

#### 🟢 Docker Compose — Admin SO 上传

```bash
# 编译出新 .so 后，通过 Admin 直接上传到共享 volume
curl -X PUT http://127.0.0.1:8090/plugins/HelloHttp/HelloHttp_ModuleHello.so \
  --data-binary @build/lib/HelloHttp_ModuleHello.so
# → Admin 写文件到 deploy/HelloHttp/plugins/ → 更新 etcd version
# → Manager Watch → Worker GracefulRestart → dlopen 新 .so
```

> 🔵 K8s: SO 烘焙在 Docker 镜像内（`/app/plugins/`），通过重建镜像 + 滚动更新部署。
> 🟢 Compose: SO 在宿主机 volume，Admin 直接 PUT 文件即可，无需重建镜像。

---

## 六、端口速查

| 服务 | 协议 | 端口 | 🟢 Compose | 🔵 K8s |
|------|------|------|:--:|:--:|
| HelloHttp | HTTP | 27006 | `127.0.0.1` | `192.168.3.61` (hostNetwork) |
| HelloHttps | HTTPS | 27443 | `127.0.0.1` | `192.168.3.61` (hostNetwork) |
| HelloWs | WebSocket | 27010 | `127.0.0.1` | `192.168.3.61` (hostNetwork) |
| HelloWss | WSS | 27012 | `127.0.0.1` | `192.168.3.61` (hostNetwork) |
| Interface | HTTP | 27008 | `127.0.0.1` | `192.168.3.61` (hostNetwork) |
| Logic v1 | S2S | 16068 | `127.0.0.1` | ClusterIP (`10.244.x.x`) |
| Logic v2 | S2S | 16069 | `127.0.0.1` | ClusterIP (`10.244.x.x`) |
| etcd | gRPC | 12379/2379 | `127.0.0.1:12379` | `thunder-etcd.thunder:2379` |
| Admin Web | HTTP | 8090 | `127.0.0.1:8090` | ClusterIP |

---

## 七、清理

```bash
./deploy.sh clean               # 🟢🔵 清理 build/ + Docker 临时数据

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
