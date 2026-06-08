# 15 — SO 模块热更新支持 etcd 管理下发

> 2026-06-09 | issus #45 | 状态: 设计中

## 背景

Thunder 的模块系统通过 `conf/*.json` 中的 `module` 配置指定要加载的 SO 插件：

```json
"module": [
  {
    "url_path": "/hello/hello",
    "so_path": "plugins/ModuleHello.so",
    "entrance_symbol": "create",
    "load": true,
    "version": 1
  }
]
```

当前更新 SO 模块的方式：

| 方式 | 命令 | 问题 |
|------|------|------|
| SIGUSR1 | `kill -SIGUSR1 <pid>` | 触发 `CmdReloadSo`，重新 dlopen SO |
| SIGUSR2 | `kill -SIGUSR2 <pid>` | Worker 优雅重启 |
| `node.sh reload` | 脚本封装 kill | 同上 |
| node.sh restart | 停→启 | 有停机窗口 |

**痛点**：
1. 需要 SSH 到机器手动操作
2. 无版本管理，不知道当前加载的是哪个版本
3. 无回滚能力
4. 无法通过 etcd Admin 界面统一管理

## 设计

### 存储

SO 二进制文件存储于共享文件系统（hostPath / NFS / 对象存储），etcd 只管理元数据：

```
  /thunder/config/{IP:PORT}  →  {
    "custom": { ... },
    "modules": [
      {
        "url_path": "/hello/hello",
        "so_path": "plugins/ModuleHello.so",
        "version": 2,
        "load": true
      }
    ]
  }
```

- `so_path`: SO 文件路径（相对于节点的 `deploy/{NodeType}/` 目录）
- `version`: 版本号，Manager 比对当前 Worker 加载版本
- `load`: 是否启用

### 更新流程

```
  1. 新 SO 放到共享目录
     deploy/HelloHttp/plugins/ModuleHello_v2.so

  2. Admin 页面修改配置
     改 version: 1 → 2
     改 so_path: "plugins/ModuleHello.so" → "plugins/ModuleHello_v2.so"

  3. 保存 → etcd
     PUT /thunder/config/10.42.0.109:27007

  4. Manager config watch 检测到变更
     OnCenterEvent("custom config updated")
     → 解析 modules 字段
     → 对比当前 Worker 加载的 SO 列表

  5. 检测到版本变更
     → 触发 GracefulRestartWorker
     → 复用 #2 的 drain 机制:
        - 新 Worker 启动 → dlopen ModuleHello_v2.so
        - 旧 Worker EnterDrainMode → 等请求完成 → 退出

  6. 热更新完成，无停机
```

### 回滚

```
  Admin 页面 → 版本历史 → 选旧版本 → 回滚
  → etcd value 恢复为旧 modules 配置
  → Manager watch 检测到变更
  → 再次触发 GracefulRestartWorker
  → 加载回旧版 SO
```

### Manager 改动点

```cpp
// Manager::OnCenterEvent — 新增 modules 变更处理
if (event.type == CenterEventType::CustomConfigUpdated) {
    auto& custom = event.custom_config;
    // 现有: 处理 custom 字段
    ApplyCustomConfig(custom);

    // 新增: 处理 modules 字段
    if (custom.HasMember("modules")) {
        auto& newModules = custom["modules"];
        if (ModulesChanged(newModules)) {
            for (int i = 0; i < m_uiWorkerNum; i++) {
                GracefulRestartWorker(i);  // 复用已有机制
            }
        }
    }
}
```

### 安全考虑

| 风险 | 缓解 |
|------|------|
| SO 文件不存在 | Manager 检测 dlopen 失败 → 保留旧 Worker → 日志告警 |
| 错误 SO 导致 Worker 崩溃 | 新 Worker 启动失败 → Manager 不杀旧 Worker → 告警 |
| 频繁重启 | 加最小间隔（如 30s 内不重复触发） |
| 版本回退 | 版本历史保留，回滚即改 etcd value |
| SO 文件安全 | 共享目录权限控制，Admin 页面不直接上传文件 |

### 与现有 Admin 页面的关系

```
  Admin 页面 → 节点配置 Modal → textarea 编辑 JSON:
  {
    "custom": { "https": {...} },
    "modules": [
      {"url_path": "/hello/hello", "so_path": "plugins/ModuleHello.so", "version": 2}
    ]
  }
  → 💾 保存 → etcd → Manager watch → GracefulRestartWorker
```

---

## k8s vs 裸机 SO 更新方案

### k8s 推荐: 重建镜像 + Rolling Update

```
  Admin 改 etcd 版本号
       │
       ▼
  CI/CD: docker build (新 SO) → docker push
       │
       ▼
  kubectl set image deploy/hello hello=thunder-hello:v2
       │
       ▼
  k8s Rolling Update:
    Pod-v2 (新SO) 启动 → healthy → Pod-v1 终止
    逐步替换, 全程无停机
```

**优点**：
- k8s 原生机制，不需要自己实现热更新
- 镜像不可变，回滚 = `kubectl rollout undo`
- 健康检查保证新版本可用才切流量

### 裸机推荐: 共享存储 + etcd 触发

```
  NFS / hostPath 共享目录: /data/thunder/plugins/
       │
       ├──► 机器A: deploy/HelloHttp/plugins → symlink → /data/thunder/plugins/
       ├──► 机器B: deploy/HelloHttp/plugins → symlink → /data/thunder/plugins/
       └──► 机器C: deploy/HelloHttp/plugins → symlink → /data/thunder/plugins/
       
  更新: 新 SO 放到 /data/thunder/plugins/ → Admin 改 etcd → 所有机器 GracefulRestart
```

| 环境 | SO 文件存放 | 触发更新 | 适用 |
|------|-----------|---------|------|
| k8s | Docker 镜像内 | `kubectl rollout restart` | ✅ 推荐 |
| k8s + hostPath | 宿主机目录 | etcd watch → GracefulRestart | 开发/测试 |
| k8s + PVC/NFS | 共享存储卷 | etcd watch → GracefulRestart | 多节点 |
| 裸机 + NFS | 共享存储 | etcd watch → GracefulRestart | ✅ 推荐 |
| 裸机 + 本地文件 | 每台机器手动放 | etcd watch → GracefulRestart | 小规模 |

---

## 零中断 Rolling Update

标准 k8s Rolling Update 的问题：

```
  Pod-v1 收到 SIGTERM → 立即杀死 → 在途请求丢失
  Pod-v2 启动中 → 尚未 Ready
  → 短暂中断窗口
```

Thunder 的解决：**preStop hook 触发 Drain + GracefulRestartWorker**

```yaml
# k8s deployment 配置
spec:
  template:
    spec:
      terminationGracePeriodSeconds: 60   # 最多等 60s
      containers:
      - name: hello
        lifecycle:
          preStop:
            exec:
              command:
              - /bin/sh
              - -c
              - |
                # 1. 触发 Worker 排空 (等请求完成, 不发 SIGTERM)
                kill -SIGUSR2 $(pgrep Hello_robot)
                # 2. 等 Worker 排空完成 (最多 50s)
                for i in $(seq 1 50); do
                  if ! pgrep Hello_robot_W0 > /dev/null 2>&1; then break; fi
                  sleep 1
                done
```

```
  kubelet 发 SIGTERM
       │
       ▼
  preStop hook:
    → kill -SIGUSR2 → Manager 触发 GracefulRestartWorker
    → 新 Worker 启动 (dlopen 新 SO)
    → 旧 Worker EnterDrainMode → 等请求完成 → 退出
       │
       ▼
  Readiness probe 失败 (旧 Worker 退出了)
    → Service 摘除旧 Pod
       │
       ▼
  新 Worker Ready → Readiness probe 通过
    → Service 加入新 Pod
       │
       ▼
  ✅ 零中断, 在途请求全部完成
```

### 对比

| 方式 | 中断 | 实现复杂度 | 适用 |
|------|------|-----------|------|
| k8s Rolling Update 裸用 | 有中断窗口 | 低 | 可接受短暂中断 |
| k8s + preStop drain | **零中断** ✅ | 中 | 生产推荐 |
| etcd 触发 GracefulRestart | **零中断** ✅ | 高 | 裸机推荐 |

---

## 前置条件: SO 必须先到位

GracefulRestartWorker 是 `dlopen(so_path)` — 从磁盘加载 SO。SO 不在磁盘上，reload 就失败。

```
  正确顺序:
  ① SO 文件到位 (镜像/共享存储)  →  ② etcd 版本号更新  →  ③ GracefulRestartWorker
  
  错误顺序:
  ① etcd 版本号更新  →  ② GracefulRestartWorker  →  dlopen 失败 (文件还不存在)
```

| 环境 | ① SO 如何到位 | ② 谁触发更新 | ③ 谁执行 reload |
|------|-------------|------------|---------------|
| k8s | 新镜像自动带新 SO | kubelet (Rolling Update) | preStop hook → GracefulRestartWorker |
| 裸机+NFS | 手动 scp 到共享目录 | Admin 改 etcd | Manager watch → GracefulRestartWorker |

---

## 更正: k8s 是换 Pod, 不是进程内 reload

```
  k8s Rolling Update 的本质:
  
  Image v1 (SO v1)              Image v2 (SO v2)
  ┌──────────────┐              ┌──────────────┐
  │ Pod-old       │    换 Pod    │ Pod-new       │
  │ dlopen v1.so │  ────────►  │ dlopen v2.so │
  └──────────────┘              └──────────────┘

  etcd 触发的进程内 reload (仅裸机):
  
  同一个进程
  ┌─────────────────────────────┐
  │ Hello_robot                  │
  │  Manager watch → 检测到变更   │
  │  → GracefulRestartWorker     │
  │  旧 Worker (v1.so) → drain   │
  │  新 Worker (v2.so) → 接替    │
  └─────────────────────────────┘
```

**结论**: k8s 不需要 etcd SO 管理。etcd SO 管理只适用于裸机/VM 部署。

---

## 焦点: SO 版本管理, 不是节点部署

```
  etcd = SO 版本管理中心
  
  /thunder/config/10.42.0.109:27007 → {"modules": [
    {"url_path": "/hello/hello", "so_path": "plugins/ModuleHello.so", "version": 2}
  ]}
  /thunder/config/10.42.0.113:27444 → {"modules": [
    {"url_path": "/hello/hello", "so_path": "plugins/ModuleHello.so", "version": 3}
  ]}
  
  节点 A 加载 v2, 节点 B 加载 v3 — etcd 统一管控
```

### SO 文件存储 (共享, 所有节点可访问)

```
  NFS / hostPath: /data/thunder/plugins/
  ├── ModuleHello_v1.so    ← 所有版本保留
  ├── ModuleHello_v2.so
  └── ModuleHello_v3.so
  
  k8s:  PV/PVC 或 hostPath 挂载到 Pod
  裸机: NFS mount 或本地路径
```

### etcd 只管版本号, 不管文件传输

```
  Admin 页面 → 改 version: 2 → 3
       │
       ▼
  etcd 存储新版本号
       │
  Manager watch → 检测到 version 变更
       │
  验证 so_path 文件存在
       │
  GracefulRestartWorker
       │
  新 Worker dlopen(...v3.so) → 旧 Worker drain
       │
  ✅ 零中断 SO 版本切换
```

---

## SO 版本管理: 按节点类型, 不按单个节点

同类节点用同一批 SO，版本一致:

```
  /thunder/config/HELLO     → modules: [ModuleHello v2, ModuleShake v1]
  /thunder/config/LOGIC     → modules: [CmdGetToken v3]
  /thunder/config/INTERFACE → modules: [ModuleInterface v1]
```

**下发**: 该类型所有节点 watch 同一个 key → 统一 GracefulRestart。

**好处**:
- 一个类型一个配置，不用每个节点配一遍
- 灰度: 改一个节点配 v3，其余保持 v2 → 验证后全部升 v3
- 跟现在 `deploy/HelloHttp/conf/Hello.json` 里的 module 配置结构一致

---

## 最终设计: SO 模块 + 节点配置

### etcd Key 结构

```
  /thunder/config/
  ├── module/                    ← SO 版本 (按类型, 同类共用)
  │   ├── HELLO     → {"modules": [{"url_path":"/hello/hello","so_path":"plugins/ModuleHello_v2.so","version":2}]}
  │   ├── LOGIC     → {"modules": [{"cmd":10001,"so_path":"plugins/CmdGetToken_v3.so","version":3}]}
  │   └── INTERFACE → {"modules": [{"url_path":"/Interface/gentoken","so_path":"plugins/ModuleInterface_v1.so","version":1}]}
  │
  ├── 10.42.0.109:27007  → {"https":{...}}           ← 节点 custom (按节点)
  └── 10.42.0.113:27444  → {"https":{...}}           ← 节点 custom (按节点)
```

| Key | 粒度 | 内容 | Watch 行为 |
|-----|------|------|-----------|
| `/thunder/config/module/{TYPE}` | 按节点类型 | SO 模块列表 | 版本变更 → GracefulRestartWorker |
| `/thunder/config/{IP:PORT}` | 按节点 | custom JSON | 变更 → 热更新 |

### SO 文件存储 (NFS 共享)

```
  NFS: 192.168.3.100:/data/thunder/plugins/
  ├── HelloHttp/
  │   ├── ModuleHello_v1.so
  │   └── ModuleHello_v2.so
  ├── Logic/
  │   ├── CmdGetToken_v2.so
  │   └── CmdGetToken_v3.so
  └── Interface/
      └── ModuleInterface_v1.so

  所有节点 mount 同一个 NFS:
  节点A deploy/HelloHttp/plugins → symlink → NFS HelloHttp/
  节点B deploy/HelloHttp/plugins → symlink → NFS HelloHttp/
```

### Manager watch 两个前缀

```cpp
// EtcdCenterConnector config watch:
//   prefix: /thunder/config/
//   收到 PUT 事件 → 解析 key 路径
//     /thunder/config/module/{TYPE} → 比对 SO 版本 → GracefulRestartWorker
//     /thunder/config/{IP:PORT}      → 热更新 custom
```

### Admin 页面

```
  🖥 节点 tab:
  ┌──────────┬─────────────────┬─────────┬────────┬──────────┬─────────┐
  │ 类型      │ 节点             │ Node ID │ Worker │ 模块      │ 配置     │
  ├──────────┼─────────────────┼─────────┼────────┼──────────┼─────────┤
  │ HELLO    │ 3 个节点          │ —       │ —      │ ⚙ 模块   │ —       │
  │ HELLO    │ 10.42.0.109:27007│ 89      │ 1      │ —        │ ⚙ 配置  │
  │ HELLO    │ 10.42.0.113:27444│ 90      │ 1      │ —        │ ⚙ 配置  │
  │ INTERFACE│ 10.42.0.114:27009│ 87      │ 1      │ —        │ ⚙ 配置  │
  │ LOGIC    │ 10.42.0.112:16068│ 88      │ 1      │ —        │ ⚙ 配置  │
  └──────────┴─────────────────┴─────────┴────────┴──────────┴─────────┘
  
  类型行 "⚙ 模块" → 管理 /thunder/config/module/{TYPE} → SO 版本
  节点行 "⚙ 配置" → 管理 /thunder/config/{IP:PORT}      → custom JSON
```

---

## SO 文件分发: URL 方式 (替代 NFS/PV)

SO 文件不在 etcd 存储，etcd 只存下载 URL。Manager 从 HTTP 服务器下载 SO。

### etcd Key

```
  /thunder/config/module/HELLO → {
    "module": [{
      "url_path": "/hello/hello",
      "so_path": "plugins/ModuleHello.so",
      "so_url": "http://192.168.3.61:8080/plugins/ModuleHello_v2.so",
      "version": 2,
      "sha256": "abc123..."    // 可选: 校验
    }]
  }
```

### Manager 下载流程

```
  ① Admin 改版本 + so_url
  ② Manager watch → 检测 version 变更
  ③ HTTP GET so_url → 写 /data/thunder/plugins/ModuleHello_v2.so
  ④ (可选) sha256 校验
  ⑤ dlopen("/data/thunder/plugins/ModuleHello_v2.so")
  ⑥ GracefulRestartWorker → 零中断切换
```

### 文件服务器

可用现有的 python HTTP server (8080 端口):
```
  deploy/Interface/confweb/plugins/ModuleHello_v2.so
```

或独立文件服务器 (nginx/minio/S3)。

### k8s PV 多节点共享 SO 分析

k8s PV 要支持多 Pod 跨节点共享 SO 文件，需要 `ReadWriteMany` 访问模式。

| PV 类型 | ReadWriteMany | 多节点 | 能用? | 原因 |
|---------|:---:|:---:|:---:|------|
| hostPath | ❌ | ❌ | ❌ | 绑定单节点磁盘, Pod 调度到其他节点不可见 |
| local | ❌ | ❌ | ❌ | 同上, 且 Pod 必须调度到 PV 所在节点 |
| Longhorn | ❌ | ❌ | ❌ | 块存储, 只支持 ReadWriteOnce |
| NFS | ✅ | ✅ | ✅ | ReadWriteMany, 多 Pod 跨节点共享 |
| CephFS | ✅ | ✅ | ✅ | ReadWriteMany, 但需 Ceph 集群 |

**结论: 只有 NFS 和 CephFS 支持多节点共享。其余全是单节点。**

### 实际方案

| 环境 | SO 分发方式 | 原因 |
|------|-----------|------|
| 单节点 k8s | hostPath | Pod 总在同一节点 |
| 多节点 k8s (有 NFS) | NFS PV ✅ | ReadWriteMany |
| 多节点 k8s (无 NFS) | HTTP URL 下载 ✅ | 不依赖存储 |
| 任何环境 | 镜像内置 + Rolling Update | 最简单 |

### NFS 部署 (k8s 多节点首选)

```
  NFS Server (一台机器或 NAS)
       │
  ┌────┴────┬────────┬────────┐
  Node-1    Node-2   Node-3   (k8s 节点)
  Pod-A     Pod-B    Pod-C    (所有 Pod 挂同一 NFS)
```

**为什么 NFS 最常用**:
- Linux 内核自带 NFS 客户端，不用装驱动
- 原生支持 ReadWriteMany
- 各种 NAS/云存储都兼容 NFS 协议
- `nfs-subdir-external-provisioner` 一行 Helm 装好 CSI

```bash
# 安装 NFS CSI provisioner
helm install nfs-subdir nfs-subdir-external-provisioner \
  --set nfs.server=192.168.3.100 \
  --set nfs.path=/data/thunder/plugins
```

**云上对应**:

| 云 | 服务 | 协议 |
|----|------|------|
| 阿里云 | NAS | NFS |
| AWS | EFS | NFS |
| 腾讯云 | CFS | NFS |
| 自建 | NFS Server / TrueNAS (仅 ~15MB 内存) | NFS |

### NFS vs CephFS 对比

两者都支持 ReadWriteMany，是多节点 SO 共享唯二选择。

| 维度 | NFS | CephFS |
|------|-----|--------|
| 协议 | NFS v3/v4 | Ceph 专有协议 |
| 客户端 | 内核自带, 零安装 | 需 ceph-common + 内核模块 |
| 部署复杂度 | ⭐ 一台 NFS Server | ⭐⭐⭐⭐ Ceph 集群 (MON+OSD+MDS) |
| k8s CSI | `nfs-subdir-external-provisioner` | `ceph-csi` |
| 性能 | 中等, 适合小文件 | 高, 适合大文件/高并发 |
| 容量 | 单机磁盘 | 横向扩展 PB 级 |
| 运维 | 简单, 单点 (可 HA) | 复杂, 需专人维护 |
| 适合 SO 场景 | ✅ 推荐 | ⚠️ 杀鸡用牛刀 |

**结论: SO 文件一般几 MB，几十个节点并发读，NFS 完全够用。CephFS 适合 PB 级数据场景，对 SO 管理来说太重。**

### NFS 服务器搭建

k8s 内置 nfs PV 类型，但需要外部 NFS 服务器提供共享目录。

```bash
# 任意 Linux 机器 (可以是 k8s 某个节点)
apt install nfs-kernel-server
mkdir -p /data/thunder/plugins
echo "/data/thunder/plugins *(ro,sync,no_subtree_check)" >> /etc/exports
exportfs -a

# Pod 挂载 (k8s 原生, 零安装)
volumes:
- name: plugins
  nfs:
    server: 192.168.x.x       # NFS 服务器 IP
    path: /data/thunder/plugins
    readOnly: true
```

**云上免自建**:
| 云 | 服务 |
|----|------|
| 阿里云 | NAS (开箱即用 NFS) |
| AWS | EFS |
| 腾讯云 | CFS |
