# Thunder 快速上手

> 更新: 2026-07-31 | 🟢 = Docker Compose | 🔵 = K8s

---

## 一、构建

> 🟢🔵 通用

```bash
# 首次
git submodule update --init --recursive

# 一条命令完成: cmake configure → 三方库 → 主工程 → 安装二进制
./deploy.sh build
```

---

## 二、启动

### 🟢 Docker Compose

```bash
./deploy.sh up          # 启动全部服务（etcd×3 + MinIO + MySQL + Redis + 7 个 Thunder 节点 + admin-web）
./deploy.sh status      # 查看容器状态 + 监听端口
./deploy.sh restart     # 重启所有容器
./deploy.sh down        # 停止并清理
```

等待约 20 秒，所有服务 healthy 后测试。

### 🔵 Kubernetes

```bash
# 一键发布（编译 + 镜像 + 部署 + 全量回归 79 项）
./deploy.sh release k8s

# 或分步：
./deploy.sh build                          # cmake 编译
./deploy.sh image all                      # 构建全部 Docker 镜像
./deploy.sh deploy                         # 导入 containerd + 滚动更新
bash k8s/regression-test.sh                # 回归测试（79 项）
```

---

## 三、测试

### 🔵 K8s 回归测试

```bash
bash k8s/regression-test.sh          # 全量 79 项 (core + admin API + canary)
bash k8s/regression-test.sh --quick  # 仅核心 52 项
bash k8s/regression-test.sh --all    # 含扩缩容/稳定性
```

覆盖：CoreDNS / 5 网关 Pod / 插件隔离 / DNS / 服务直连 / SO 热更新 / RBAC / 节点调优 / 全链路 / admin-web API 16 项 / 灰度路由 11 项

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

admin-web → `📜 Lua` tab：在线编辑脚本 → 保存 → 自动热加载。

详见 [`k8s/README.md#lua-热更新`](k8s/README.md#lua-热更新)

### SO 热更新（#159 Pull 模式）

> 🟢🔵 通用 — 同一套链路：上传 → MinIO → etcd bump → Manager HTTP Pull → Worker dlopen

**Web 界面**：admin-web → `📦 插件` tab：上传 .so → 选版本 → 点"下发" → 自动部署。

**API 方式**：

```bash
# 1. 上传 .so 到制品库（自动存入 MinIO + 本地 artifacts）
curl -X PUT http://127.0.0.1:8090/api/plugins/Logic/CmdGetToken.so \
  --data-binary @./CmdGetToken.so

# 2. 下发到目标节点（etcd bump 版本号 → Manager Pull 下载 → Worker 热加载）
curl -X POST http://127.0.0.1:8090/api/plugins/Logic/deploy \
  -H "Content-Type: application/json" \
  -d '{"filename":"CmdGetToken.so"}'

# 3. 查看已部署的 SO 列表及版本
curl http://127.0.0.1:8090/api/plugins/Logic/deployed

# 4. 🔵 K8s 下验证 SO 已到达 Pod
kubectl exec -n thunder deploy/thunder-logic -- ls -la /app/plugins/
```

**全链路**: `PUT .so` → MinIO `artifacts/{Type}/{file}` → admin-web etcd bump `so_url` + `version++` → 各节点 Manager watch 感知 → `DownloadSoFile(so_url)` HTTP GET MinIO → 原子写入本地插件目录 → Worker `dlopen()` 加载 → 零停机热更新完成。

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
| Admin Web (Go) | HTTP | 8090 | `127.0.0.1:8090` | NodePort 30090 |
| MinIO API | HTTP | 9000 | `127.0.0.1:9000` | `thunder-minio.thunder:9000` |
| MinIO Console | HTTP | 9001 | `127.0.0.1:9001` | NodePort 30091 |

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
| Canary 灰度路由 | `docs/architecture/17-k8s-canary-routing.md` |
| entrypoint + Compose | `docs/architecture/19-entrypoint-and-docker-compose-canary.md` |
| K8s 运维 | `k8s/k8s-manual.md` |
| SO 热更新 via etcd | `docs/architecture/08-so-module-hot-reload-via-etcd.md` |
| Lua 模块 | `docs/architecture/11-lua-send-to-node-type.md` |
| Manager/Worker IPC | `docs/architecture/06-manager-worker-ipc.md` |
