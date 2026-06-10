# 16 — Admin Web 组件设计

> 2026-06-10 | 部署路径: `deploy/admin-web/`

## 1. 概述

Admin Web 是 Thunder 的运维管理界面，单文件静态 HTML + Python 服务端，部署在 k8s 集群内。

**核心功能**：
- 🖥 节点管理 — 查看注册节点、编辑节点配置、版本历史/回滚
- 📦 SO 镜像管理 — 列出 SO 镜像、提取 .so 文件、触发热更新
- 📊 集群状态 — etcd 健康检查、节点/配置计数

## 2. 文件结构

```
deploy/admin-web/
├── index.html       ← 管理页面 (单文件 SPA, 直连 etcd API)
├── server.py        ← HTTP 服务端 (静态文件 + API + SO 提取)
├── Dockerfile       ← k8s 部署镜像
├── share_so.sh      ← 编译机临时分享 SO 脚本
├── plugins/         ← SO 文件存储 (本地缓存, 上传/提取目标)
└── .gitignore
```

## 3. 架构

```
浏览器                         k8s Pod (thunder-admin-web)
┌──────────┐  静态文件          ┌─────────────────────────────┐
│ index.html│ ◄── index.html ── │ server.py :8090              │
│          │                   │                             │
│ fetch()  │── etcd API ──────►│ etcd:2379  (直连, 不代理)    │
│          │                   │                             │
│ fetch()  │── so-images ────►│ docker.sock → 列出/提取镜像  │
│          │── so-files  ────►│                             │
│          │── so-extract ───►│                             │
└──────────┘                   │ 写入 → plugins/ (本地)       │
                               │ 写入 → /data/thunder/plugins/│
                               └─────────────────────────────┘
```

## 4. server.py API

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/index.html` | 管理页面 |
| GET | `/api/so-images` | 列出可用 SO 镜像 (docker images) |
| GET | `/api/so-files?image=xxx` | 列出镜像内 .so 文件 |
| POST | `/api/so-extract` | 提取 SO: docker create → get_archive → 写本地+NFS |
| PUT | `/plugins/{TypeDir}/{file}.so` | 上传 SO 文件 |

**请求格式**：
```json
POST /api/so-extract
{"image": "so-hellohttp_modulehello:latest", "file": "HelloHttp_ModuleHello.so", "type": "HELLO_HTTP"}
```

**响应**：
```json
{"ok": true, "path": "/plugins/HelloHttp/HelloHttp_ModuleHello.so", "size": 4850952}
```

## 5. 数据流

```
SO 提取请求
  → server.py
  → docker.from_env() (docker.sock)
  → containers.create(image)
  → get_archive("/app/so/file.so")
  → tar 解包
  → 写入 plugins/{TypeDir}/file.so (本地)
  → 写入 /data/thunder/plugins/{TypeDir}/file.so (NFS)
  → 返回 {ok, size}
```

## 6. k8s 部署

```yaml
# k8s/admin-web-deployment.yaml
image: python:3.12-alpine
command: ["sh", "-c", "pip install -q docker; python3 server.py --port 8090"]
ports: [8090]
volumes:
  - hostPath: /home/tommychen/thunder/deploy/admin-web → /app
  - hostPath: /data/thunder/plugins → /data/thunder/plugins (NFS)
  - hostPath: /var/run/docker.sock → /var/run/docker.sock
service:
  type: NodePort
  port: 8090 → nodePort: 30090
```

## 7. 安全

| 风险 | 措施 |
|------|------|
| docker.sock 暴露 | 仅内网可访问, 后续加 RBAC |
| etcd 直连 | ?etcd= 参数控制, 无凭证 |
| SO 文件写入 | NFS 目录权限控制 |
| CORS | `Access-Control-Allow-Origin: *` (内网可接受) |
