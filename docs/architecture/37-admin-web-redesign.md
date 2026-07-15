# Thunder 管理后台重构设计（参考 Nacos）

> 设计日期：2026-07-15  |  状态: 📋 设计阶段  |  关联: #134 加权路由、#135 Python CLI、Center Admin
>
> **目标：重构 deploy/admin-web/，达到 Nacos 级别的管理体验。同时充分复用 Center Admin 已验证的设计模式（单文件 SPA、CSS 变量体系、tab 导航）。**

---

## 0. 现状

### 0.1 两套 Admin 并存

| | Center Admin | admin-web |
|---|---|---|
| **路径** | `deploy/Center/conf/admin/AdminPage.html` | `deploy/admin-web/server.py` + `index.html` |
| **形式** | 单文件 HTML，内嵌于 Center 进程 | 独立 Python http.server 进程 |
| **后端** | Center 内置 `/admin` HTTP API | server.py 自己连 etcd |
| **能力** | 服务列表、集群命令（show/set/get/config） | SO 上传、Lua 管理、etcd 读写 |
| **设计** | ✅ CSS 变量、tab SPA、现代配色（#1677ff） | ❌ 裸 JS 堆砌、无导航结构 |
| **鉴权** | 无（标注"仅内网使用"） | 无 |

**关键判断**：Center Admin 是已验证的良好模式——单文件 SPA + Center 内置 API。admin-web 重构应继承这个模式，而非另起炉灶。

### 0.2 admin-web 具体问题

```
deploy/admin-web/
├── server.py        ← 473 行，混杂静态服务 + SO上传 + Lua管理 + etcd代理
├── index.html        ← 无框架，裸 HTML + 内嵌 JS，难以扩展
├── plugins/          ← SO 上传目录
└── Testing/          ← 测试文件
```

| 问题 | 表现 |
|---|---|
| 无导航结构 | 所有功能挤在一个页面，靠堆砌 tab/按钮区分 |
| 无组件化 | HTML/JS/CSS 混在一起，加一个功能要改 300 行 |
| 无状态管理 | 页面数据靠全局变量，刷新全丢 |
| 交互粗糙 | 没有确认弹窗、没有加载状态、没有错误提示 |
| 后端耦合 | API 逻辑内嵌在 http.server 的 do_GET/do_POST 里 |

---

## 1. 设计目标

对标 Nacos 管理后台的核心体验，复用 Center Admin 已验证模式：

| Nacos 做的 | Thunder 怎么做 |
|---|---|
| 服务列表卡片化（一眼看到实例数和健康状态） | 从 etcd `/thunder/registry/` 读节点，按 node_type 分组卡片展示 |
| 实例详情（IP、端口、版本、元数据） | 解析 etcd registry JSON，展示 node_version/worker_num/心跳 |
| 配置管理（在线编辑、diff 对比） | etcd config 功能，UI 重构为专业编辑器 |
| 权重调整（拖滑块） | 等 #134 完成，滑块写 `/thunder/canary/` 权重键 |
| 操作确认（防误操作） | 每次写 etcd 前弹确认框，显示变更前后对比 |
| 单文件 SPA + tab 导航 | 继承 Center Admin 的 tab SPA 模式 |

---

## 2. 设计系统

> 继承 Center Admin 已验证的 CSS 变量体系，统一扩展为完整设计系统。

### 2.1 色彩

```css
:root {
  /* 主色 - 保持与 Center Admin 一致 */
  --primary:        #1677ff;
  --primary-hover:  #4096ff;
  --primary-active: #0958d9;
  --primary-bg:     #e6f4ff;

  /* 中性色 */
  --bg:             #f0f2f5;
  --bg-elevated:    #ffffff;
  --border:         #e8e8e8;
  --border-light:   #f0f0f0;

  /* 文字 */
  --text:           #1f1f1f;
  --text-secondary: #666666;
  --text-muted:     #999999;
  --text-inverse:   #ffffff;

  /* 状态色 */
  --success:        #52c41a;
  --success-bg:     #f6ffed;
  --warning:        #faad14;
  --warning-bg:     #fffbe6;
  --error:          #ff4d4f;
  --error-bg:       #fff2f0;
  --info:           #1677ff;
  --info-bg:        #e6f4ff;

  /* 排版 */
  --radius-sm:      6px;
  --radius:         8px;
  --radius-lg:      12px;
  --shadow-sm:      0 1px 2px rgba(0,0,0,.04);
  --shadow:         0 2px 8px rgba(0,0,0,.08);
  --shadow-lg:      0 4px 16px rgba(0,0,0,.12);

  /* 间距 */
  --space-xs:       4px;
  --space-sm:       8px;
  --space:          12px;
  --space-md:       16px;
  --space-lg:       24px;

  /* 字阶 */
  --text-xs:        11px;
  --text-sm:        13px;
  --text-base:      14px;
  --text-lg:        16px;

  /* 布局 */
  --max-width:      1200px;
  --header-h:       48px;
  --nav-h:          40px;
}

/* 暗色主题 */
[data-theme="dark"] {
  --bg:             #141414;
  --bg-elevated:    #1f1f1f;
  --border:         #303030;
  --border-light:   #262626;
  --text:           #e0e0e0;
  --text-secondary: #999999;
  --text-muted:     #666666;
}
```

### 2.2 组件规范

统一组件 class 命名，保证全站视觉一致：

```html
<!-- 按钮 -->
<button class="btn">默认按钮</button>
<button class="btn btn-primary">主按钮</button>
<button class="btn btn-ghost">幽灵按钮</button>
<button class="btn btn-danger">危险按钮</button>
<button class="btn btn-sm">小按钮</button>
<button class="btn" disabled>禁用</button>

<!-- 卡片 -->
<div class="card">
  <div class="card-header">标题</div>
  <div class="card-body">内容</div>
</div>

<!-- 表单 -->
<input class="input" type="text" placeholder="输入...">
<select class="select">...</select>
<textarea class="textarea" rows="4"></textarea>

<!-- 表格 -->
<table class="table">
  <thead><tr><th>列1</th><th>列2</th></tr></thead>
  <tbody>...</tbody>
</table>

<!-- 徽标 -->
<span class="badge badge-success">在线</span>
<span class="badge badge-error">离线</span>
<span class="badge badge-default">v1</span>

<!-- Toast 通知 -->
<div class="toast toast-success">操作成功</div>
<div class="toast toast-error">操作失败</div>

<!-- 弹窗 -->
<div class="modal-overlay">
  <div class="modal">
    <div class="modal-header">确认操作</div>
    <div class="modal-body">...</div>
    <div class="modal-footer">
      <button class="btn btn-ghost">取消</button>
      <button class="btn btn-primary">确认</button>
    </div>
  </div>
</div>
```

### 2.3 基础 CSS（tailwind-lite）

不引入 Tailwind CDN（避免外部依赖），自建精简 utility class 集合：

```css
/* 布局 */
.flex { display: flex; }          .flex-col { flex-direction: column; }
.flex-wrap { flex-wrap: wrap; }   .flex-1 { flex: 1; }
.items-center { align-items: center; }
.justify-between { justify-content: space-between; }
.justify-center { justify-content: center; }
.gap-xs { gap: var(--space-xs); } .gap-sm { gap: var(--space-sm); }
.gap { gap: var(--space); }       .gap-md { gap: var(--space-md); }

/* 间距 */
.m-0 { margin: 0; }              .mt { margin-top: var(--space); }
.mb { margin-bottom: var(--space); }
.p-sm { padding: var(--space-sm); }
.p { padding: var(--space); }

/* 文字 */
.text-sm { font-size: var(--text-sm); }
.text-muted { color: var(--text-muted); }
.text-error { color: var(--error); }
.text-success { color: var(--success); }
.text-center { text-align: center; }
.font-mono { font-family: ui-monospace, monospace; }
.font-bold { font-weight: 600; }

/* 可见性 */
.hidden { display: none; }
.truncate { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
```

---

## 3. 架构

```
┌─ 前端 (单文件 SPA) ───────────────────────────────────────────────┐
│  static/index.html                                           │
│                                                               │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐              │
│  │ 概览  │ │ 节点  │ │ 灰度  │ │ 配置  │ │ 插件  │  ← Tab │
│  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘              │
│                                                               │
│  前端 API 层: 统一的 fetch 封装 (api.js)                       │
│  状态管理:   页面级 state 对象 (state.js)                      │
│  组件库:     复用 components.js (Modal / Toast / Card)         │
│                                                               │
└──────────────────────────┬───────────────────────────────────────────────┘
                           │ REST API
┌─ 后端 API ───────────────┼───────────────────────────────────────────────┐
│  server.py (重构后)                                           │
│                                                               │
│  GET  /api/overview                    → 集群概览              │
│  GET  /api/nodes?node_type=LOGIC       → 节点列表              │
│  GET  /api/nodes/:id                    → 节点详情              │
│  GET  /api/canary/:service/weights      → 读取灰度权重          │
│  POST /api/canary/:service/weights      → 写入灰度权重          │
│  GET  /api/config/:type                → 读取配置              │
│  PUT  /api/config/:type                → 更新配置              │
│  POST /api/plugins/upload              → 上传插件              │
│  GET  /api/plugins                     → 插件列表              │
│                                                               │
│  etcd_client.py ─ etcd v3 API 封装（从 server.py 拆出）        │
│                                                               │
│  所有端点最终操作 etcd──Admin 是 Proxy，不是新数据源             │
│                                                               │
└───────────────────────────────────────────────────────────────────────┘
```

---

## 4. 页面设计

> 继承 Center Admin 的 tab SPA 模式。5 个 tab 对应 5 个 panel，单 HTML 文件完成。

### 4.1 概览页

```
+-- Thunder Admin ----------------------------------------------------+
|  [概览]  [节点]  [灰度]  [配置]  [插件]                              |
+----------------------------------------------------------------------+
|                                                                      |
|  +-- 集群状态 ---------------------------------------------------+   |
|  |  etcd: 已连接    节点: 12    在线: 10    离线: 2                |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
|  +-- 服务分布 ---------------------------------------------------+   |
|  |                                                                  |   |
|  |  +- LOGIC ---------------------------------------------------+  |   |
|  |  | 在线 3 实例  v1x2  v2x1    全部在线                       |  |   |
|  |  +------------------------------------------------------------+  |   |
|  |  +- INTERFACE -----------------------------------------------+  |   |
|  |  | 在线 2 实例  v1x2           全部在线                       |  |   |
|  |  +------------------------------------------------------------+  |   |
|  |  +- HELLO_HTTP ----------------------------------------------+  |   |
|  |  | 在线 2 实例  v1x2           全部在线                       |  |   |
|  |  +------------------------------------------------------------+  |   |
|  |                                                                  |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
|  自动刷新: 10s  [立即刷新]                                           |
|                                                                      |
+----------------------------------------------------------------------+
```

### 4.2 节点管理页

```
+-- 节点管理 -----------------------------------------------------------+
|                                                                      |
|  服务: [LOGIC v]  [搜索 IP...]  [搜索]                               |
|                                                                      |
|  +- v1 (x2) -----------------------------------------------------+   |
|  |  +----------+  +----------+                                    |   |
|  |  | 在线     |  | 在线     |                                    |   |
|  |  |          |  |          |                                    |   |
|  |  | 10.0.0.1 |  | 10.0.0.2 |                                    |   |
|  |  | :16001   |  | :16002   |                                    |   |
|  |  | node_id 2|  | node_id 3|                                    |   |
|  |  | Workerx4 |  | Workerx4 |                                    |   |
|  |  | 2min ago |  | 30s ago  |                                    |   |
|  |  |          |  |          |                                    |   |
|  |  | [详情]   |  | [详情]   |                                    |   |
|  |  +----------+  +----------+                                    |   |
|  +----------------------------------------------------------------+   |
|                                                                      |
|  +- v2 (x1) -----------------------------------------------------+   |
|  |  +----------+                                                  |   |
|  |  | 在线     |                                                  |   |
|  |  | 10.0.0.3 |                                                  |   |
|  |  | :16003   |                                                  |   |
|  |  | Workerx4 |                                                  |   |
|  |  | [详情]   |                                                  |   |
|  |  +----------+                                                  |   |
|  +----------------------------------------------------------------+   |
|                                                                      |
+----------------------------------------------------------------------+

点击 [详情] -> 弹窗展示完整注册 JSON
```

### 4.3 灰度管理页

> 依赖 #134 完成

```
+-- 灰度管理 -----------------------------------------------------------+
|                                                                      |
|  服务: [LOGIC v]                                                     |
|                                                                      |
|  +- 权重分配 ---------------------------------------------------+   |
|  |                                                                  |   |
|  |  v1  ------o--------------------  70%                           |   |
|  |  v2  ------------o--------------  30%                           |   |
|  |                                                                  |   |
|  |  [应用] [重置] [一键回滚]                                        |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
|  +- 变更预览 ---------------------------------------------------+   |
|  |  v1: 80% -> 70%  (v10%)                                         |   |
|  |  v2: 20% -> 30%  (^10%)                                         |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
+----------------------------------------------------------------------+
```

### 4.4 配置管理页

```
+-- 配置管理 -----------------------------------------------------------+
|                                                                      |
|  模块: [LOGIC v]   类型: [Logic.json v]                              |
|                                                                      |
|  +- 当前配置 ---------------------------------------------------+   |
|  |  +--------------------------------------------------------+     |   |
|  |  | {                                                      |     |   |
|  |  |   "node_type": "LOGIC",                                 |     |   |
|  |  |   "listen_port": 16001,                                 |     |   |
|  |  |   ...                                                   |     |   |
|  |  | }                                                      |     |   |
|  |  +--------------------------------------------------------+     |   |
|  |  revision: 15   更新时间: 2026-07-15 10:30                     |   |
|  |  [编辑] [下载]                                                 |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
+----------------------------------------------------------------------+

点击 [编辑] -> 同级页面内展开 JSON 编辑器 -> [保存]时弹 diff 确认
```

### 4.5 插件管理页

```
+-- 插件管理 -----------------------------------------------------------+
|                                                                      |
|  +- 上传插件 ---------------------------------------------------+   |
|  |  拖拽 .so 文件到此处 或 [选择文件]                               |   |
|  |  类型: [INTERFACE v]   版本: [v1 v]                              |   |
|  |  [上传并部署]                                                    |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
|  +- 已部署插件 -------------------------------------------------+   |
|  |  名称        类型        版本    大小    时间    [操作]          |   |
|  |  hello.so    Logic       v1     1.2M   10:30  [回滚]           |   |
|  |  world.so    Interface   v2     0.8M   09:15  [回滚]           |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
+----------------------------------------------------------------------+
```


### 4.6 日志/审计页

```
+-- 审计日志 -----------------------------------------------------------+
|                                                                      |
|  时间范围: [2026-07-15] ~ [2026-07-15]  操作: [全部 v]  [搜索]       |
|                                                                      |
|  +----------------------------------------------------------------+   |
|  | 时间           操作      目标         变更摘要                    |   |
|  | 10:30:15       权重变更  LOGIC         v1:80->70  v2:20->30      |   |
|  | 09:15:42       配置更新  LOGIC.json    revision 14->15           |   |
|  | 09:14:30       插件部署  hello.so      Logic v1  已部署          |   |
|  | 08:00:00       节点上线  10.0.0.1:16001 LOGIC v1 node_id=2      |   |
|  +----------------------------------------------------------------+   |
|                                                                      |
|  点击行 -> 弹窗展示完整 before/after JSON diff                        |
|                                                                      |
+----------------------------------------------------------------------+
```

### 4.7 配置历史版本（配置管理页子功能）

```
+-- 配置管理 / 历史版本 -----------------------------------------------+
|                                                                      |
|  模块: [LOGIC v]   类型: [Logic.json v]   当前 revision: 15          |
|                                                                      |
|  +----------------------------------------------------------------+   |
|  | revision   更新时间       变更大小   操作                        |   |
|  | 15         07-15 10:30    +12B      [查看] [对比当前] [回滚]    |   |
|  | 14         07-14 16:20    -8B       [查看] [对比当前] [回滚]    |   |
|  | 13         07-13 09:00    +45B      [查看] [对比当前] [回滚]    |   |
|  | ...                                                             |   |
|  +----------------------------------------------------------------+   |
|                                                                      |
|  Diff 视图:                                                          |
|  +----------------------------------------------------------------+   |
|  | - "listen_port": 16000,    (revision 14)                       |   |
|  | + "listen_port": 16001,    (revision 15)                       |   |
|  +----------------------------------------------------------------+   |
|                                                                      |
+----------------------------------------------------------------------+
```

### 4.8 节点详情（结构化面板）

```
+-- 节点详情: LOGIC / 10.0.0.1:16001 ----------------------------------+
|                                                                      |
|  +- 基本信息 ---------------------------------------------------+   |
|  |  node_type    LOGIC                    状态    在线            |   |
|  |  node_id      2                        版本    v1              |   |
|  |  IP:Port      10.0.0.1:16001           Worker  4              |   |
|  |  最后心跳     2026-07-15 10:30:25      (30s ago)               |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
|  +- 元数据 -----------------------------------------------------+   |
|  |  {                                                              |   |
|  |    "custom_field_1": "value1",                                  |   |
|  |    "custom_field_2": "value2"                                   |   |
|  |  }                                                              |   |
|  +-----------------------------------------------------------------+   |
|                                                                      |
|  [查看原始 JSON]  [关闭]                                             |
|                                                                      |
+----------------------------------------------------------------------+
```
---

## 5. 交互规范

所有影响 etcd 的操作遵循同一套防误操作流程：

```
操作按钮 -> 变更预览弹窗（展示 before/after diff）-> 确认 -> 执行 -> Toast 反馈
```

| 规范 | 说明 |
|---|---|
| 加载态 | 所有 fetch 操作显示 loading spinner（inline 或全局） |
| 错误态 | API 调用失败显示 toast 通知（红色，4s 自动消失） |
| 成功态 | 写入成功显示 toast 通知（绿色，3s 自动消失） |
| 确认弹窗 | 权重变更、删除配置等破坏性操作必须弹窗确认 |
| 灰度滑块 | 拖拽时实时预览百分比，不直接提交；需点 [应用] 才生效 |
| 自动刷新 | 概览页/节点页 10s 自动刷新，可暂停 |
| 空状态 | 列表为空时显示友好提示，非白屏 |
| 键盘 | Esc 关闭弹窗，Enter 确认 |

### 5.1 Toast 组件

```js
// 全局唯一，页面顶部居中，自动消失
function showToast(message, type, duration) { /* type: success|error|warning|info */ }
showToast('权重已更新', 'success');
showToast('连接 etcd 失败', 'error');
```

### 5.2 Modal 组件

```js
// 确认弹窗，支持自定义标题/内容/按钮
function showModal({ title, body, confirmText, cancelText, onConfirm, danger }) { }
showModal({
  title: '确认修改权重',
  body: '<pre>v1: 80% -> 70%  (v10%)</pre>',
  confirmText: '确认',
  onConfirm: () => submitWeights(),
});
```

---

## 6. 前端结构

```
static/
├── index.html          ← 入口，tab 导航框架 + 5 个 panel
├── css/
│   └── admin.css       ← 设计系统（§2 全部 CSS）
└── js/
    ├── api.js           ← 前端 API 封装（fetch 包装，统一错误处理）
    ├── state.js         ← 页面级状态管理（当前 tab、数据缓存）
    ├── components.js    ← 通用组件（Toast / Modal / Card / Badge / Loading）
    ├── overview.js      ← 概览页逻辑
    ├── nodes.js         ← 节点页逻辑
    ├── canary.js        ← 灰度页逻辑
    ├── config.js        ← 配置页逻辑
    └── plugins.js       ← 插件页逻辑
```

所有 JS 通过 `<script src="js/xxx.js"></script>` 加载。文件名即命名空间，无模块打包。

### 6.1 命名约定

```js
// state.js — 全局状态
var STORE = {
  tab: 'overview',
  nodes: { data: [], loading: false, error: null },
  autoRefresh: null,
};

// components.js — 全局组件
var Toast = { show: function(){} };
var Modal = { show: function(){} };

// 页面逻辑 — IIFE，不污染全局
(function() {
  // overview.js 的代码
})();
```

---

## 7. API 约定

### 7.1 通用格式

```
Request:  GET/POST/PUT + JSON body
Response: { "ok": true, "data": {...} }  |  { "ok": false, "error": "..." }
HTTP Code: 200 (业务错误走 response body，不用 HTTP 错误码)
```

### 7.2 端点

#### 概览

```
GET /api/overview
-> {
    "ok": true,
    "data": {
      "etcd_connected": true,
      "total_nodes": 12,
      "online_nodes": 10,
      "services": [
        {"node_type": "LOGIC", "count": 3, "online": 3, "versions": {"v1": 2, "v2": 1}},
        {"node_type": "INTERFACE", "count": 2, "online": 2, "versions": {"v1": 2}}
      ]
    }
  }
```

#### 节点

```
GET /api/nodes?node_type=LOGIC
-> {
    "ok": true,
    "data": {
      "node_type": "LOGIC",
      "nodes": [
        {
          "ip": "10.0.0.1", "port": 16001, "node_id": 2,
          "version": "v1", "worker_num": 4, "online": true,
          "active_time": "2026-07-15T10:30:00"
        }
      ]
    }
  }
```

#### 灰度权重

```
GET /api/canary/LOGIC/weights
-> {
    "ok": true,
    "data": {
      "service": "LOGIC",
      "active": true,
      "weights": {"v1": 80, "v2": 20},
      "total": 100
    }
  }

POST /api/canary/LOGIC/weights
Body: {"weights": {"v1": 70, "v2": 30}}
-> {
    "ok": true,
    "data": {
      "previous": {"v1": 80, "v2": 20},
      "current": {"v1": 70, "v2": 30}
    }
  }
```

#### 配置

```
GET /api/config/LOGIC?type=Logic.json
-> { "ok": true, "data": { "content": {...}, "revision": 15 } }

PUT /api/config/LOGIC
Body: {"type": "Logic.json", "content": {...}}
-> { "ok": true, "data": { "revision": 16 } }
```

#### 插件

```
GET /api/plugins
-> { "ok": true, "data": { "plugins": [{"name": "hello.so", "type": "Logic", ...}] } }

POST /api/plugins/upload
Body: FormData { file, node_type, version }
-> { "ok": true, "data": { "name": "hello.so" } }
```

---

## 8. 后端拆分

```
deploy/admin-web/
├── server.py               ← 入口：路由分发 + 静态文件 serve
├── api/
│   ├── __init__.py
│   ├── overview.py          ← /api/overview
│   ├── nodes.py             ← /api/nodes
│   ├── canary.py            ← /api/canary
│   ├── config.py            ← /api/config
│   └── plugins.py           ← /api/plugins
├── etcd_client.py           ← etcd v3 API 封装（从 server.py 拆出，支持读写 + prefix scan）
├── static/                  ← 前端文件（见 §6）
└── plugins/                 ← SO 文件存储（已有）
```

---

## 9. 实现计划

| 阶段 | 内容 | 预估 | 依赖 |
|:---:|---|:---:|---|
| **P0** | ① 设计系统落地（admin.css: 色彩 + 组件 + utilities） | 1d | 无 |
| | ② 通用组件 JS（Toast / Modal / Loading / Card） | | |
| | ③ 导航框架（index.html: tab SPA + panel 切换） | | |
| **P1** | 后端 API 重构（server.py 拆出 etcd_client + 路由模块） | 1d | 无 |
| **P2** | 概览页 + 节点管理页 | 2d | P0, P1 |
| **P3** | 灰度管理页（权重滑块 + 变更预览，依赖 #134） | 1d | #134 |
| **P3.5** | 配置历史版本（history key + 列表 + diff 视图） | 0.5d | P1, P4 |
| **P4** | 配置管理页 + 插件管理页重构 | 2d | P0, P1 |
| **P5** | 审计日志（拦截层 + 列表页 + 详情 diff） | 1d | P1 |
| **P5.5** | 交互规范落地（确认弹窗/Toast/Loading 全站统一） | 1d | P0 |
| **P6** | 暗色主题（基于 §2.1 CSS 变量切换） | 0.5d | P0 |

**P0+P1 不依赖 #134，可立即开始。总计 ~9d。**

---

## 10. 与 Center Admin 的分工

| | Center Admin | admin-web（重构后） |
|---|---|---|
| **定位** | Center 集群运维面板 | 全功能管理后台 |
| **后端** | Center 内置 /admin API | 独立 server.py -> etcd |
| **服务列表** | ✅ node_type 聚合 | ✅ 节点详情卡片 |
| **集群命令** | ✅ show/set/get/config | ❌（属于 Center Admin 特有） |
| **灰度权重** | ❌ | ✅（#134 完成后） |
| **插件管理** | ❌ | ✅（SO 上传/部署/回滚） |
| **配置编辑** | ❌（仅 Center 自身配置） | ✅（etcd 全局配置） |
| **JSON 控制台** | ✅（Center Admin 特色） | ❌ |

> 两套 Admin 互补，不互相替代。Center Admin 专注 Center 集群运维，admin-web 专注业务层管理。

---

## 11. 参考

- Center Admin 源码：`deploy/Center/conf/admin/AdminPage.html`
- Nacos 控制台设计参考
- 现有 admin-web：`deploy/admin-web/server.py`
- 对比分析：`docs/center_stagement.md`
- 问题清单：#139 条目位于 `issus-list.md` 5381 行


---


## 12. 存储方案：etcd + SQLite 分层

### 12.1 Nacos 怎么做

Nacos 的存储是分层的，不会把所有东西塞进 Raft 引擎：

```
Nacos:
  服务注册表    -> 内存 + Raft/Distro（CP 协议，强一致）
  配置当前值    -> MySQL / 内嵌 Derby
  配置历史版本  -> MySQL / 内嵌 Derby（his_config_info 表）
  审计日志      -> MySQL / 内嵌 Derby（Nacos 2.x+）

  关键：配置及其历史走外部数据库，不走 Raft。
```

### 12.2 Thunder 对标方案

admin-web 是**单例进程**（只有一个实例在跑），不存在并发写冲突。直接用 Python 标准库 `sqlite3`（零新依赖，跟 `import json` 一个性质）。

| 数据类型 | 存储 | 理由 |
|---|---|---|
| 节点/注册信息 | **etcd** | Center 管理的，admin 只读 |
| 配置当前值 | **etcd** | 一致性要求 |
| 配置历史版本 | **SQLite** | append 写入，按 revision 查；不占 etcd 空间 |
| 审计日志 | **SQLite** | append 写入，支持时间范围 SQL 查询 |

> **这才是真正对标 Nacos**：Raft 管服务注册一致性，SQLite 管历史/审计持久化。etcd 不塞日志数据。

### 12.3 SQLite 方案细节

```python
import sqlite3

# server.py 启动时初始化（自动建库建表，零运维）
DB_PATH = Path(__file__).parent / 'admin.db'

def init_db():
    db = sqlite3.connect(str(DB_PATH))
    db.execute('''
        CREATE TABLE IF NOT EXISTS config_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            config_key TEXT NOT NULL,
            revision INTEGER NOT NULL,
            content TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    db.execute('''
        CREATE TABLE IF NOT EXISTS audit_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            action TEXT NOT NULL,
            target TEXT NOT NULL,
            before_value TEXT,
            after_value TEXT,
            client_ip TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    db.commit()
    return db

# 配置写入带历史
def config_put_with_history(db, key, new_value):
    old = _etcd_get(key)
    if old and old != new_value:
        db.execute(
            'INSERT INTO config_history (config_key, revision, content) VALUES (?, ?, ?)',
            (key, _next_revision(db, key), old)
        )
        db.commit()
    return _etcd_put(key, new_value)

# 审计日志
def audit_log(db, action, target, before, after, ip):
    db.execute(
        'INSERT INTO audit_log (action, target, before_value, after_value, client_ip) VALUES (?, ?, ?, ?, ?)',
        (action, target, json.dumps(before), json.dumps(after), ip)
    )
    db.commit()
```

### 12.4 SQLite vs etcd 在 admin 单例场景下的对比

| | etcd | SQLite |
|---|---|---|
| 写路径 | Raft 共识 -> 多数派确认 | 本地文件 fsync |
| 查询 | prefix scan，不支持 SQL | SELECT WHERE ts BETWEEN ... |
| 清理 | 依赖 TTL Lease | DELETE WHERE ts < '...' |
| 数据膨胀 | 占 etcd 内存/磁盘，影响集群 | 独立 admin.db，隔离 |
| 备份 | 混在 etcd 快照里 | 单独拷贝 admin.db |
| 新依赖 | 无（etcd 已有） | 无（Python 标准库） |

### 12.5 可行性总结

| 功能 | 难度 | 后端改动 | 存储 | Nacos 对标 |
|---|---|---|---|---|
| 节点详情结构化 | 🟢 低 | 无 | — | ✅ |
| 配置历史版本 | 🟡 中 | ~20 行 | SQLite config_history | ✅ his_config_info |
| 审计日志 | 🟡 中 | ~20 行 | SQLite audit_log | ✅ Nacos 2.x+ |

> **三项均可实现。全部零新依赖。总后端改动 ~40 行。真正对标 Nacos 的分层存储设计。**

