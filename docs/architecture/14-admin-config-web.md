# 14 — Admin 配置管理 Web 界面设计

> 2026-06-09 | issus #41, #42, #43, #44

## 背景

Thunder 使用 etcd 作为注册中心和配置下发通道。节点运行时配置（`custom` 字段）可通过 etcd watch 机制热更新到各节点。但缺少 Web 界面来查看和修改这些配置，运维只能通过 etcdctl 或 curl 命令行操作。

## 设计目标

1. **纯静态** — 单个 HTML 文件，浏览器直接打开，直连 etcd REST API，零后端依赖
2. **一个节点一个 key** — 不拆字段，整个 `custom` JSON 作为一个 value 存储
3. **入口在节点列表** — 每行节点加"⚙ 配置"按钮，点击弹 Modal 编辑
4. **版本历史** — 每次保存自动备份旧值，支持回滚

---

## etcd Key 设计

```
/thunder/config/{IP:PORT}         →  {"https":{...},"log_level":"INFO"}
/thunder/config_history/{IP:PORT}/v{timestamp_ms}  →  旧版 JSON
```

| Key | 内容 | 说明 |
|-----|------|------|
| `/thunder/config/10.42.0.113:27444` | 完整 custom JSON | 当前生效配置 |
| `/thunder/config_history/10.42.0.113:27444/v1780918634360` | 旧版 JSON | 历史版本 |

**为什么不用分层 key**:
```
  ❌ /thunder/config/{TYPE}/{IP:PORT}/{sub_key}  — 太复杂，需要多级选择器
  ✅ /thunder/config/{IP:PORT}                     — 一个节点一个 key，value 就是完整 JSON
```

节点类型通过 etcd registry (`/thunder/registry/{TYPE}/{IP:PORT}`) 获取，不需要在 config key 中冗余。

---

## 页面交互

### 节点列表 (主页面)

```
  🖥 节点 tab
  ┌──────────┬─────────────────┬─────────┬────────┬─────────┐
  │ 类型      │ 节点 IP:Port      │ Node ID │ Worker │ 配置     │
  ├──────────┼─────────────────┼─────────┼────────┼─────────┤
  │ HELLO    │ 10.42.0.109:27007│ 89      │ 1      │ ⚙ 配置  │
  │ HELLO    │ 10.42.0.113:27444│ 90      │ 1      │ ⚙ 配置  │
  │ INTERFACE│ 10.42.0.114:27009│ 87      │ 1      │ ⚙ 配置  │
  │ LOGIC    │ 10.42.0.112:16068│ 88      │ 1      │ ⚙ 配置  │
  └──────────┴─────────────────┴─────────┴────────┴─────────┘
```

- 数据来源: `POST /v3/kv/range` on `/thunder/registry/`
- 每行 "⚙ 配置" 按钮 → 打开该节点的配置 Modal

### 配置 Modal

```
  点击 "⚙ 配置" →
  ┌──────────────────────────────────────────────┐
  │  ⚙ HELLO 10.42.0.113:27444 custom 配置       │
  │  etcd key: /thunder/config/10.42.0.113:27444 │
  │                                                │
  │  ┌──────────────────────────────────────┐      │
  │  │ {                                      │      │
  │  │   "https": {                           │      │
  │  │     "server": {                        │      │
  │  │       "cert_file": "conf/certs/...",  │      │
  │  │       "key_file":  "conf/certs/...",  │      │
  │  │       "verify_client": false           │      │
  │  │     },                                  │      │
  │  │     "client": {                         │      │
  │  │       "verify_peer": true               │      │
  │  │     }                                   │      │
  │  │   }                                     │      │
  │  │ }                                       │      │
  │  └──────────────────────────────────────┘      │
  │                                                │
  │  📋 版本历史: v3(当前) v2 v1                    │
  │                                     [💾 保存]  │
  └──────────────────────────────────────────────┘
```

- 打开时: GET `/thunder/config/{IP:PORT}` 读取当前 JSON
- 编辑: textarea 直接编辑完整 JSON
- 保存: 
  1. 读旧值 → PUT `/thunder/config_history/{IP:PORT}/v{ts}` (备份)
  2. PUT `/thunder/config/{IP:PORT}` (新值)
- 版本历史: GET `/thunder/config_history/{IP:PORT}/` prefix range
- 回滚: GET 历史版本 → PUT `/thunder/config/{IP:PORT}`

---

## 版本历史 & 回滚

```
  版本历史 panel:
  ┌──────────────┬─────────────────────┬─────────┐
  │ 版本          │ 预览                 │ 操作     │
  ├──────────────┼─────────────────────┼─────────┤
  │ v1780918...  │ {"https":{"server":.│ ↩ 回滚  │
  │ v1780918...  │ {"https":{"server":.│ ↩ 回滚  │
  │ v1780918...  │ {"log_level":"INFO"} │ ↩ 回滚  │
  └──────────────┴─────────────────────┴─────────┘
```

- 版本号使用毫秒时间戳，保证唯一且有序
- 预览显示前 60 字符
- 回滚: confirm → PUT 当前 key → 刷新

---

## 配置下发流程

配置保存在 etcd，通过两层机制同步到所有节点。

### 第一层：etcd 集群内部同步 (Raft)

```
  Admin → PUT /thunder/config/10.42.0.113:27444
              │
              ▼
         etcd-0  ── Raft ── etcd-1  ── Raft ── etcd-2
              │                 │                 │
              └─────────────────┴─────────────────┘
                    所有 etcd 节点数据一致

  k8s:   单节点 etcd-local 或 3 副本 etcd-statefulset
  裸机:  多 etcd 逗号分隔 + 自动故障转移 (#40)
```

### 第二层：etcd → Thunder 节点 (Config Watch 下发)

```
  etcd 数据变更 (PUT /thunder/config/{IP:PORT})
       │
       ▼
  EtcdWatcher (每个节点的 Manager 进程内)
    prefix watch: /thunder/config/
    检测到 PUT 事件
       │
       ▼
  Manager::OnCenterEvent(CenterEventType::CustomConfigUpdated)
       │
       ▼
  共享内存 (ShmRingQueue)
    写入 config snapshot
       │
       ▼
  Worker::CheckShareMem()
    定时检查 → 发现新版本 → 读取 JSON
       │
       ▼
  应用到运行时:
    - Hello:  更新 https/custom 配置 → 无需重启
    - Interface: 更新 upstream_types/超时/日志级别
    - Logic:   更新 SO 模块/超时参数
```

**关键点**：
- 每个节点的 Manager 独立 watch `/thunder/config/` 前缀
- 变更通过共享内存推送给本节点的 Worker
- 热更新，不重启进程
- 下发延迟: etcd watch 实时 + Manager 处理 (~ms级) + Worker CheckShareMem 轮询间隔

---

## 访问方式

| 环境 | 页面 URL | etcd 地址 |
|------|---------|----------|
| 本地 | `file:///.../confweb/index.html` | `127.0.0.1:2379` |
| 内网 | `http://192.168.3.61:8080/index.html?etcd=192.168.3.61:30079` | NodePort 30079 |
| k8s 内 | port-forward + 页面 | `thunder-etcd.thunder:2379` |

---

## 文件

| 文件 | 说明 |
|------|------|
| `deploy/admin-web/index.html` | 管理页面 (纯静态) |
| `deploy/admin-web/plugins/` | SO 文件服务器 |
| `code/Interface/src/ModuleHello/ModuleInterface.cpp` | 不参与 admin (已解耦, #43) |

## 依赖

- etcd v3 REST API (gRPC-gateway)
- 浏览器 ES6 (fetch, btoa/atob, TextEncoder)
- 无后端依赖，无构建工具
