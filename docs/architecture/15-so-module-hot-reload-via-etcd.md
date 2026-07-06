# 15 — SO 模块热更新 via etcd

> 2026-06-09 | 更新: 2026-07-04 (#132 废弃镜像提取，统一直接上传) | 状态: ✅ 已实现

## 1. 问题

SO 模块更新需手动替换文件 + 发信号，无版本管理，无回滚，无法远程操作。

## 2. 设计概览

```
远程开发机                           服务器
┌──────────────────┐          ┌─────────────────────────────────────┐
│ 浏览器 Admin 页面 │          │  upload_server.py :8090             │
│                  │   PUT    │  ┌─────────────────────────────┐    │
│  📤 上传 .so     │ ───────► │  │ admin-web/plugins/{Type}/   │    │
│  ✏  编辑版本     │          │  │ /data/thunder/plugins/{Type}/│    │
│  💾 保存         │   POST   │  └─────────────────────────────┘    │
│  ↩  回滚         │ ───────► │                                     │
│                  │          │  etcd :2379                         │
└──────────────────┘          │  /thunder/config/module/{TYPE}       │
                              │         │                            │
                              │         ▼ watch                      │
                              │  Manager::OnCenterEvent              │
                              │    ├─ 版本比对                       │
                              │    ├─ DownloadSoFile (如有 so_url)   │
                              │    └─ GracefulRestartWorker          │
                              │         │                            │
                              │    ┌────┴────┐                      │
                              │    ▼         ▼                      │
                              │  新Worker  旧Worker                  │
                              │  dlopen    Drain→exit               │
                              │  接管      零中断 ✅                  │
                              └─────────────────────────────────────┘
```

## 3. etcd Key 结构

```
/thunder/config/
├── module/                          ← SO 版本 (按节点类型)
│   ├── HELLO     → {"module":[{so_path, version, so_url?}]}
│   ├── LOGIC     → {"module":[{cmd, so_path, version}]}
│   └── INTERFACE → {"module":[{url_path, so_path, version}]}
│
├── {IP}:{PORT}                      ← 节点 custom JSON
│
└── config_history/module/{TYPE}/v{timestamp}  ← 版本历史 (自动备份)
```

## 4. SO 文件存储

```
deploy/{Type}/plugins/    ← 标准路径，所有环境统一
├── HelloHttp/ModuleHello.so
├── HelloWs/CmdHello.so
├── Logic/CmdGetToken.so
└── Interface/ModuleInterface.so
```

### 部署方式（#132 统一为直接上传）

**唯一入口**：`PUT /plugins/{Type}/{filename}` — 直接上传 .so 文件。

| 环境 | 存储 | 工作机制 |
|------|------|---------|
| Docker Compose | 本地 `deploy/` | 全项目挂载 → 所有容器共享同一文件系统 |
| K8s | NFS `/data/thunder/plugins/` | admin-web 写 NFS → 所有 Pod mount 同一卷 |

```
Docker Compose:                           K8s:
  PUT .so                                    PUT .so
    │                                          │
    ▼                                          ▼
  deploy/HelloHttp/plugins/xxx.so          NFS /data/thunder/plugins/HelloHttp/xxx.so
    │                                          │
    ├─ admin-web 容器可见                      ├─ admin-web Pod mount 可见
    └─ hello 容器可见 (同一挂载)                └─ 所有服务 Pod mount 可见
                                                (NFS 是网络文件系统，Pod mount 后像本地盘一样直接读，不需要下载)
```

### 为什么废弃 Docker 镜像提取 (#132)

| 问题 | 说明 |
|------|------|
| 过度包装 | 3MB alpine 镜像只含 1MB .so，拉取→创建容器→提取→删除，纯浪费 |
| 安全风险 | admin-web 需挂载 `/var/run/docker.sock`（root 权限） |
| 无意义绕圈 | cmake 已产出 .so，Docker 包一层再解包 |
| 复杂度 | 需要 registry + build-so + pull + extract，vs 一行 curl PUT |

## 5. 热更新流程

```
cmake → .so → curl PUT :8090/plugins/{Type}/{file}
  │
  ├─ 写本地: deploy/{Type}/plugins/xxx.so      (Docker Compose 直接可见)
  └─ 写 NFS:  /data/thunder/plugins/{Type}/xxx.so  (K8s 所有 Pod 可见)
  │
  ▼
更新 etcd /thunder/config/module/{TYPE} 版本号
  │
  ▼
Manager Watch 检测变更 → GracefulRestartWorker → dlopen(m_strWorkPath + so_path)
                                                   ↑
                                              Docker Compose: /thunder/deploy/{Type}/plugins/
                                              K8s: 需要在 deployment 中把 NFS 挂到同路径（当前缺口）
```

| 优势 | 说明 |
|------|------|
| 版本即镜像 tag | `v1`, `v2`, `v3` 天然版本管理 |
| 不重建 Pod | 节点镜像不变, SO 热加载 |
| 回滚简单 | Admin 选旧版本 → 从旧镜像提取 → 热加载 |
| CI/CD 友好 | `docker build && docker push` 标准流程 |

## 6. 上传服务 (upload_server.py)

```
deploy/upload_server.py --port 8090

接收:  PUT /plugins/{TypeDir}/{filename}.so
写入:  ① deploy/admin-web/plugins/{TypeDir}/{filename}.so  (本地)
       ② /data/thunder/plugins/{TypeDir}/{filename}.so    (NFS, k8s 共享)

CORS:  Access-Control-Allow-Origin: *  (支持远程浏览器上传)
```

## 7. Admin 页面 (deploy/admin-web/index.html)

```
🖥 节点 tab — 按类型分组

类型头行:  HELLO (3节点)  [⚙ 模块]
  ├─ Modal: SO 模块配置
  │   ┌──────────────────────────────────────┐
  │   │  📤 上传 SO:  [选择文件] [⬆ 上传]     │  ← 远程上传
  │   │  ─────────────────────────────────── │
  │   │  {                                   │
  │   │    "module": [{                      │
  │   │      "so_path": "plugins/xxx_v2.so", │  ← 自动填入
  │   │      "version": 2,                   │  ← 自动 +1
  │   │      "so_url": "http://..."          │  ← 可选
  │   │    }]                                │
  │   │  }                                   │
  │   │  ─────────────────────────────────── │
  │   │  [💾 保存] [📋 版本历史]              │
  │   └──────────────────────────────────────┘

节点行:    10.42.0.109:27007  [⚙ 配置]
  └─ Modal: 节点 custom JSON (https 等)
```

## 8. 热更新流程 (Manager 自动)

```
etcd PUT /thunder/config/module/{TYPE}
    │
    ▼
EtcdCenterConnector watch 检测变更
    │
    ▼
Manager::OnCenterEvent(ConfigUpdated)     Manager.cpp:2735
    │
    ├── 解析 JSON, 比较 so_path/version    Manager.cpp:2743-2777
    ├── 有 so_url → DownloadSoFile()       Manager.cpp:2775-2797
    └── 版本变更 → GracefulRestartWorker   Manager.cpp:2794-2796
            │
            ├── fork 新 Worker → dlopen(新SO) → WORKER_READY
            └── 旧 Worker EnterDrainMode → 等请求 → exit(0)
```

## 9. 回滚

```
Admin → ⚙ 模块 → 📋 版本历史 → 选版本 → ↩ 回滚
    │
    ▼
etcd PUT 旧配置 → Manager 检测变更 → GracefulRestartWorker → 加载旧 SO
```

## 10. k8s 多节点支持

```yaml
# k8s/plugins-pv.yaml — NFS PersistentVolume
apiVersion: v1
kind: PersistentVolume
spec:
  nfs:
    server: 192.168.3.100
    path: /data/thunder/plugins
  accessModes: [ReadOnlyMany]

# 所有 Deployment 挂载:
volumes:
  - name: thunder-plugins
    persistentVolumeClaim:
      claimName: thunder-plugins
volumeMounts:
  - name: thunder-plugins
    mountPath: /data/thunder/plugins
    readOnly: true
```

## 11. 关键文件

| 文件 | 职责 |
|------|------|
| `deploy/admin-web/server.py` | Admin 服务 + SO 上传/提取 (#45, #48) |
| `deploy/admin-web/index.html` | Admin 页面 (本地上传/镜像提取/模块管理/回滚) |
| `k8s/admin-web-deployment.yaml` | Admin k8s 部署 (docker.sock + NFS 挂载) |
| `code/Net/src/labor/Manager.cpp:2735-2805` | ConfigUpdated handler (版本比对+下载+重启) |
| `code/Net/src/labor/Manager.cpp:2812-2857` | GracefulRestartWorker |
| `code/Net/src/labor/Manager.cpp:2861-2921` | DownloadSoFile (HTTP 下载 SO) |
| `code/Net/src/register/EtcdCenterConnector.cpp` | PutConfig 同步模块配置到 etcd (#46) |
| `k8s/plugins-pv.yaml` | NFS PV/PVC |
| `k8s/*-deployment.yaml` | 各 Deployment 挂载 NFS PVC |

## 12. 安全

| 风险 | 措施 |
|------|------|
| 恶意 SO 上传 | NFS 目录权限; 上传服务器可加认证 |
| 版本回退 | 历史保留, 可再次回滚 |
| 频繁重启 | 仅 so_path/version 真正变化才触发 |
| 下载失败 | DownloadSoFile 失败 → 跳过重启 |
| 新 Worker 崩溃 | 保留旧 Worker, 日志告警 |

## 13. 访问地址

| 场景 | 地址 | 说明 |
|------|------|------|
| 本地 | `http://127.0.0.1:8090` | etcd 默认 `127.0.0.1:2379` |
| 远程 | `http://192.168.3.61:8090` | etcd 默认 `192.168.3.61:2379` |
| k8s NodePort | `http://192.168.3.61:30090` | etcd 默认 `192.168.3.61:2379` |

**`?etcd=` 参数**：Admin 页面默认用当前页面 IP + `:2379` 连接 etcd。若 etcd 端口不是 2379（如 k8s NodePort 30079），通过 `?etcd=` 指定：

```
http://192.168.3.61:30090/?etcd=192.168.3.61:30079
                         └──── etcd NodePort ────┘
```

---

## 14. 生产环境完整流程

```
CI/CD                              Registry                  Admin Pod                  NFS                    各 k8s Pod
─────                              ────────                  ─────────                  ───                    ─────────
docker build so-hello:v3
  │
  ├── docker push  ──────────────► registry/
  │                                so-hello:v3
  │                                    │
  │                                    │  docker pull (login registry)
  │                                    ▼
  │                                Admin Pod
  │                                ├── 提取 /app/so/*.so
  │                                ├── 写入本地 plugins/
  │                                └── 写入 NFS ──────────► /data/thunder/plugins/
  │                                                              │
  │                                              ┌───────────────┼───────────────┐
  │                                              ▼               ▼               ▼
  │                                          Pod-1 (Hello)  Pod-2 (Hello)  Pod-3 (Hello)
  │                                          mount: /data/   mount: /data/   mount: /data/
  │                                          thunder/        thunder/        thunder/
  │                                          plugins/        plugins/        plugins/
  │                                              │               │               │
  │                                              └───────────────┴───────────────┘
  │                                                      所有节点同时可见
  │
  └── Admin 页面: 填镜像名 + 文件名 → 提取 → etcd PUT
                                              │
                                         Manager watch
                                         GracefulRestartWorker
                                         Worker dlopen (NFS 路径)
                                         ⚠ NFS 文件即时可见, 但 SO 已加载到进程内存
                                         必须 GracefulRestart 才能生效
```

**NFS 原理**: 

```
Admin Pod                                 各 k8s Pod
┌──────────┐                             ┌──────────┐  ┌──────────┐
│ 写入文件  │                             │  只读挂载  │  │  只读挂载  │
│ /data/   │──── NFS 服务端 ────┬───────│ /data/   │  │ /data/   │
│ thunder/ │   192.168.3.100   │        │ thunder/ │  │ thunder/ │
│ plugins/ │                    │        │ plugins/ │  │ plugins/ │
└──────────┘                    │        └──────────┘  └──────────┘
                                │
                                │  同一个文件系统
                                │  一份数据, 多处可见
                                │  零拷贝, 无需分发
```

Admin 写入 NFS = 直接写入 NFS 服务端的磁盘。所有 Pod 通过 k8s PV (ReadOnlyMany) 挂载同一个 NFS 目录，`dlopen("/data/thunder/plugins/xxx.so")` 直接读到最新文件，无需拷贝、无需等待同步。

**⚠ 但 SO 更新后仍需 GracefulRestartWorker**：NFS 上的文件即时可见，但旧的 SO 已通过 `dlopen` 加载到 Worker 进程内存中（代码段、符号表等）。新文件放在磁盘上不会自动替换进程内的旧代码。必须通过 etcd 触发 GracefulRestartWorker → fork 新 Worker → `dlopen` 新 SO → 旧 Worker drain → 退出，才能以进程粒度完成 SO 版本切换。否则新旧 SO 内存布局不同，代码段/符号表无法对齐，直接替换会导致段错误。

```
原因：
  NFS 解决文件分发 (文件级别) ✅
    └─ Admin 写一次 → 所有 Pod 看到新 .so

  但不能解决进程热替换 (进程级别) ❌
    ┌──────────────────────┐
    │ Worker 进程地址空间    │
    │  旧 SO .text  0x7f.. │ ← dlopen 映射, 进程存活期间不变
    │  旧 SO .data  0x7f.. │ ← 全局变量/状态
    │  旧 SO 函数指针       │ ← Step 协程持有, dlclose 后悬空
    │  旧 SO vtable        │ ← 虚函数表地址固定
    └──────────────────────┘
    磁盘上的新 SO → 旧进程不可见
    dlclose + dlopen → 函数指针悬空 → segfault

解决:
  GracefulRestartWorker → fork 新进程 → dlopen 新 SO (新进程地址空间)
  → 旧进程 drain → exit → 新进程接管
```

---

## 15. SO 镜像构建与使用指南

### 目录结构 (一个模块一个目录)

```
so-images/
├── Hello_ModuleHello/
│   └── Hello_ModuleHello.so      ← 编译产出
├── Hello_ModuleShake/
│   └── Hello_ModuleShake.so
├── HelloWs_CmdHello/
│   └── HelloWs_CmdHello.so
├── HelloWs_ModuleShake/
│   └── HelloWs_ModuleShake.so
├── HelloHttps_ModuleHello/
│   └── HelloHttps_ModuleHello.so
├── Logic_CmdGetToken/
│   └── Logic_CmdGetToken.so
└── Interface_ModuleInterface/
    └── Interface_ModuleInterface.so
```

### 构建 (deploy.sh)

```bash
./deploy.sh build-so all                        # 全量构建
./deploy.sh build-so Hello_ModuleHello          # 单独构建一个模块
```

SO 无变化自动跳过。Dockerfile 自动生成。

```bash
# 手动构建 + 推送
cd so-images/Hello_ModuleHello
docker build -t registry/so-Hello_ModuleHello:v3 .
docker push registry/so-Hello_ModuleHello:v3
```

### Admin 操作

```
1. 打开: http://192.168.3.61:30090/?etcd=192.168.3.61:30079
2. 🖥 节点 → HELLO [⚙ 模块]
3. 📦 镜像列表 → 点击 so-Hello_ModuleHello:latest
4. 填文件名: Hello_ModuleHello.so → ⬇ 提取
5. 自动: pull → 提取 → NFS → etcd → GracefulRestartWorker
```
