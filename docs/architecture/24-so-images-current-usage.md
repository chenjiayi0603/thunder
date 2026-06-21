# so-images/ 当前状态与使用方式

> 日期: 2026-06-21 | 状态: ⚠️ 待验证  
> 关联优化提案: [22-so-module-distribution-optimization.md](22-so-module-distribution-optimization.md)  
> 关联热更新设计: [15-so-module-hot-reload-via-etcd.md](15-so-module-hot-reload-via-etcd.md)

---

## 1. 目录结构

```
so-images/
├── HelloHttp_ModuleHello/
│   ├── Dockerfile        ← git 追踪（固定 3 行，FROM alpine + COPY *.so + CMD）
│   ├── ModuleHello.so    ← 不入库（根 .gitignore 的 *.so 覆盖）
│   └── .so_hash          ← 不入库（增量构建缓存，so-images/.gitignore）
├── HelloHttp_ModuleRaw/
├── HelloHttps_ModuleHello/
├── HelloWs_CmdHello/
├── HelloWs_ModuleShake/
├── Interface_ModuleInterface/
├── Logic_CmdGetToken/
└── .gitignore            ← 只写了 .so_hash
```

**GitHub 上只有 Dockerfile 骨架**，.so 文件每次本地编译后才有，不会上传。

---

## 2. 完整链路

```
cmake --build
    ↓  产出 build/code/Net/.../*.so
deploy.sh build-so [模块名]
    ↓  1. 把 build/ 里对应 .so 复制到 so-images/Module/
    ↓  2. docker build -t so-<module>:latest so-images/Module/
    ↓     镜像内容只有 /app/so/xxx.so（alpine rootfs ~3.5MB + .so ~500KB）
    ↓  3. sha256 增量：.so 无变化则跳过构建
Docker 本地镜像 so-hello_modulehello:latest
    ↓
Admin Web UI（deploy/admin-web/server.py）
    ├── GET  /api/so-images  → 列出本地 docker images so-*
    └── POST /api/so-extract → docker create + get_archive + untar → 写 NFS + 通知 etcd
    ↓
etcd 通知生产节点
    ↓
生产节点 dlopen 新 .so（热更新）
```

---

## 3. 使用方式

### 3.1 构建 SO 镜像

```bash
# 全量构建（所有 7 个模块）
./deploy.sh build-so all

# 单独构建
./deploy.sh build-so HelloHttp_ModuleHello
./deploy.sh build-so Interface_ModuleInterface
./deploy.sh build-so Logic_CmdGetToken

# 查看生成的 Docker 镜像
docker images so-*
```

### 3.2 查询可用镜像（需先起 admin-web）

```bash
curl http://127.0.0.1:8090/api/so-images
```

返回示例：
```json
[
  {"image": "so-hello_modulehello:latest", "size": "4.1MB"},
  {"image": "so-interface_moduleinterface:latest", "size": "4.2MB"}
]
```

### 3.3 热更新（提取 .so 并下发）

```bash
curl -X POST http://127.0.0.1:8090/api/so-extract \
  -H 'Content-Type: application/json' \
  -d '{"image":"so-hello_modulehello:latest","file":"ModuleHello.so","type":"HELLO"}'
```

`type` 字段映射到服务类型：`HELLO`、`INTERFACE`、`LOGIC`、`HELLO_WS`、`HELLO_HTTPS`

---

## 4. 增量构建逻辑

`deploy.sh build-so` 对每个模块：
1. sha256 当前 .so
2. 对比 `.so_hash`
3. 若 hash 一致且 docker image 存在 → 输出"无变化, 跳过"
4. 否则 `docker build`，写入新 hash

首次构建后 → 全量通过；未改动的模块二次构建 → 全部跳过。

---

## 5. git 存储现状分析

| 文件 | git 状态 | 原因 |
|------|---------|------|
| `so-images/*/Dockerfile` | ✅ 追踪 | 固定骨架，供克隆新机器参考 |
| `so-images/*/*.so` | ❌ 不追踪 | 根 `.gitignore` 里 `*.so` |
| `so-images/*/.so_hash` | ❌ 不追踪 | `so-images/.gitignore` |

**Dockerfile 内容固定，7 个模块完全一样**（FROM alpine:3.20 / COPY *.so /app/so/ / CMD ["/bin/true"]），理论上可以不入库（靠 deploy.sh 动态生成）。目前保留在 git 是为了让新机器 clone 后能看到目录结构。

---

## 6. 已知问题

| 问题 | 严重程度 | 说明 |
|------|---------|------|
| admin 挂 docker.sock | HIGH | K8s 反模式，有 root 逃逸风险 |
| 镜像存储浪费 | MEDIUM | alpine rootfs 3.5MB，实际 .so 只 500KB，浪费 87% |
| 无版本回滚 | MEDIUM | Docker tag 可覆盖，无历史版本 |
| 构建机须有 Docker daemon | LOW | 本地开发和 CI 都需要 Docker |

优化方案见 [22-so-module-distribution-optimization.md](22-so-module-distribution-optimization.md)（用 MinIO 替换 Docker registry）。

---

## 7. 验证清单（待做）

- [ ] `./deploy.sh build-so all` 全量构建，7 个模块全部通过
- [ ] 二次构建无变化，7 个模块全部输出"跳过"
- [ ] `GET /api/so-images` 返回 7 个镜像列表
- [ ] `POST /api/so-extract` 提取 ModuleHello.so → 写入 deploy/ 对应目录
- [ ] etcd 通知下发 → 生产节点 dlopen 新 .so → 验证新逻辑生效
- [ ] so-images/ Dockerfile 是否还有必要入库（讨论后决定）
