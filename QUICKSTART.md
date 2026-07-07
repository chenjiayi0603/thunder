# Thunder 快速上手

---

## 一、构建

### 冷启动（首次 / 三方库未编译）

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1   # 编译三方库，约 10~20 分钟
cmake --build build -j1                               # ← 全量编译（三方库 + 主工程）
cmake --install build
```

> ⚠️ **必须 `-j1`**，多线程编译会因磁盘 IO 瓶颈卡死。

### 日常重编（三方库已就绪）

```bash
cmake --build build -j1 && cmake --install build      # ← 全量重编
```

### 只重编某模块

```bash
cmake --build build --target Net -j1                 # 网络框架核心
cmake --build build --target InterfacePlugins -j1    # Interface 插件
cmake --build build --target HelloPlugins -j1        # Hello 插件
```

### Proto 变更后

```bash
cmake --build build --target thunder_proto_gen -j1
cmake --build build -j1                              # ← 重新全量编译
```

---

## 二、启动 / 停止

### Docker Compose（本地开发 / 测试）

```bash
./deploy.sh up          # 启动集群（3-node etcd + MySQL + Redis + 全部服务）
./deploy.sh status      # 查看容器状态 + 监听端口
./deploy.sh restart     # 重启所有容器
./deploy.sh down        # 停止并清理
```

等待约 15 秒，所有服务进入 healthy 状态后再测试。

### Kubernetes（生产部署）

```bash
kubectl apply -f k8s/                                       # 部署全部服务
kubectl -n thunder rollout status deployment --timeout=120s # 等待就绪

# NodePort: HTTP=30006  Interface=30008  HTTPS=30043  WS=30010  Admin=30090
```

---

## 三、冒烟测试

```bash
./tests/test_smoke.sh
```

覆盖 HTTP / HTTPS / WebSocket / Interface→Logic 全链路（POST + GET）/ etcd，9 项全绿即通过。

---

## 四、自动化测试

```bash
# 单元测试（C++ + Python，零外部依赖，~45s）
./deploy.sh test unit

# E2E 集成测试（需 Docker，~3min）
./deploy.sh test e2e

# 全部（unit + E2E）
./deploy.sh test

# 跳过构建直接跑测试
./deploy.sh test unit --skip-build
./deploy.sh test e2e  --skip-build
```

---

## 五、清理

```bash
./deploy.sh clean       # 清理 build/ + Docker + tmp
```

---

## 端口速查

| 服务 | 协议 | 端口 |
|------|------|------|
| HelloHttp | HTTP | 27006 |
| HelloHttps | HTTPS | 27443 |
| HelloWs | WebSocket | 27010 |
| Interface | HTTP | 27008 |
| Logic | 内部 S2S | 16068 |
| Admin | HTTP | 8090 |
| etcd | HTTP | 2379 |
| Redis | TCP | 6379 |
| MySQL | TCP | 3306 |

---

## Admin 管理后台

**作用**：Web 管理界面，管理节点拓扑、SO 插件热更新、Lua 脚本下发、etcd 配置浏览。

### 访问地址

| 环境 | 地址 |
|------|------|
| Docker Compose | `http://127.0.0.1:8090` |
| K8s | `http://<node>:30090/?etcd=<etcd-ip>:30079` |

`?etcd=` 参数让浏览器直连 etcd API（静态 HTML，非服务端转发）。

### 启动

```bash
# Docker Compose（随 deploy.sh up 自动启动）
# 手动: cd deploy/admin-web && python3 server.py --port 8090

# K8s
kubectl apply -f k8s/admin-web-deployment.yaml
```

### 后台 API

| API | 方法 | 作用 |
|-----|:---:|------|
| `/api/so-images` | GET | 列出可用 SO 镜像 |
| `/api/so-files?image=xxx` | GET | 列出镜像内 .so 文件 |
| `/api/so-extract` | POST | 从镜像提取 SO → NFS 分发 → etcd 更新 → worker 热加载 |
| `/api/fetch` | POST | 从远端 URL 拉取 SO 文件 |
| `/api/lua-scripts` | GET | 列出 Lua 脚本及版本 |
| `/api/lua-scripts` | POST | 上传 Lua 脚本 → 推送到 etcd → worker 热加载 |
| `/api/sync-config` | POST | 从本地配置文件初始化 etcd 模块配置 |

### SO 模块热更新流程

```
构建 SO 镜像 → Admin 选择镜像/文件 → 提取 → NFS 分发 → etcd 版本更新
                                                       ↓
                                              GracefulRestartWorker
                                              (旧 worker 排空连接 → 新 worker 启动，零中断)
```

**构建 SO 镜像**: `./deploy.sh build-so HelloHttp_ModuleHello`  
**回滚**: 版本历史 → 选版本 → 回滚  
**节点配置**: 节点行 → 编辑 custom JSON

> 详细流程见 `docs/architecture/15-so-module-hot-reload-via-etcd.md`
