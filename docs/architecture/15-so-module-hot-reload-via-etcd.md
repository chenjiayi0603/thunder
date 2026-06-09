# 15 — SO 模块热更新 via etcd

> 2026-06-09 | issus #45 | 状态: ✅ 已实现

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
/data/thunder/plugins/    ← NFS 共享 (k8s Pod 只读挂载)
├── HelloHttp/ModuleHello.so, ModuleHello_v2.so
├── HelloWs/CmdHello.so, ModuleShake.so
├── HelloHttps/ModuleHello.so
├── Logic/CmdGetToken.so
└── Interface/ModuleInterface.so

deploy/admin-web/plugins/ ← 上传服务目录 (与 NFS 同步双写)
```

| 环境 | SO 来源 | 说明 |
|------|---------|------|
| 裸机 | 本地 `deploy/{Type}/plugins/` | dlopen 直接加载 |
| k8s | NFS `/data/thunder/plugins/` | PV ReadOnlyMany → Pod mountPath |
| URL 分发 | HTTP `so_url` → DownloadSoFile | Manager 从上传服务器下载 |

## 5. 上传服务 (upload_server.py)

```
deploy/upload_server.py --port 8090

接收:  PUT /plugins/{TypeDir}/{filename}.so
写入:  ① deploy/admin-web/plugins/{TypeDir}/{filename}.so  (本地)
       ② /data/thunder/plugins/{TypeDir}/{filename}.so    (NFS, k8s 共享)

CORS:  Access-Control-Allow-Origin: *  (支持远程浏览器上传)
```

## 6. Admin 页面 (deploy/admin-web/index.html)

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

## 7. 热更新流程 (Manager 自动)

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

## 8. 回滚

```
Admin → ⚙ 模块 → 📋 版本历史 → 选版本 → ↩ 回滚
    │
    ▼
etcd PUT 旧配置 → Manager 检测变更 → GracefulRestartWorker → 加载旧 SO
```

## 9. k8s 多节点支持

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

## 10. 关键文件

| 文件 | 职责 |
|------|------|
| `deploy/upload_server.py` | SO 上传服务 (PUT 接收, 本地+NFS 双写) |
| `deploy/admin-web/index.html` | Admin 管理页面 (上传/编辑/保存/回滚) |
| `code/Net/src/labor/Manager.cpp:2735-2805` | ConfigUpdated handler (版本比对+下载+重启) |
| `code/Net/src/labor/Manager.cpp:2812-2857` | GracefulRestartWorker |
| `code/Net/src/labor/Manager.cpp:2861-2921` | DownloadSoFile (HTTP 下载) |
| `k8s/plugins-pv.yaml` | NFS PV/PVC |
| `k8s/*-deployment.yaml` | 各 Deployment 挂载 NFS PVC |

## 11. 安全

| 风险 | 措施 |
|------|------|
| 恶意 SO 上传 | NFS 目录权限; 上传服务器可加认证 |
| 版本回退 | 历史保留, 可再次回滚 |
| 频繁重启 | 仅 so_path/version 真正变化才触发 |
| 下载失败 | DownloadSoFile 失败 → 跳过重启 |
| 新 Worker 崩溃 | 保留旧 Worker, 日志告警 |

## 12. 访问地址

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
