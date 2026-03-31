# 插件式“伪反射” vs “反射化”对比（Thunder）

本文针对当前项目 `Worker + .so` 动态加载模型，说明两种方案的差异、推荐结论与代码示例。

---

## 1. 先说结论：哪个更好？

对你这个项目，建议：

- **短期/线上稳定优先**：继续用当前“伪反射”方案（你现在就在用）
- **中长期/插件数量增加**：逐步做“反射化”改造（先兼容双栈，再迁移）

原因很直接：

- 当前方案已跑通，改动最小，排障路径清晰
- 反射化能减少配置重复、统一入口、降低人肉维护成本
- 但反射化要新增插件协议与兼容逻辑，初期改动面更大

---

## 2. 两种方案定义

### 2.1 伪反射（当前模型）

- 配置里声明：`cmd`、`so_path`、`entrance_symbol`
- `Worker` 运行时：
  - `dlopen(so_path)`
  - `dlsym(entrance_symbol)`
  - 调工厂函数创建 `Cmd*`

本质是“字符串驱动动态创建对象”，像反射，但不是语言内建反射。

### 2.2 反射化（统一插件协议）

- 每个插件 `.so` 导出统一接口（固定符号），例如 `GetPluginMeta()`
- `Worker` 固定按统一符号拿插件元数据，再按 `cmd` 找 creator
- 配置可简化为主要关注 `so_path`（必要时加白名单）

本质是把“入口细节”从配置迁到插件自描述协议。

---

## 3. 伪反射示例（对应你现在的风格）

### 3.1 配置示例

```json
{
  "so": [
    {
      "cmd": 10001,
      "so_path": "plugins/CmdGetToken.so",
      "entrance_symbol": "create",
      "load": true,
      "version": 1
    }
  ]
}
```

### 3.2 Worker 加载示例（简化）

```cpp
std::string soPath = m_strWorkPath + "/" + soConf["so_path"].asString();
std::string symbol = soConf["entrance_symbol"].asString();
int cmd = soConf["cmd"].asInt();

void* handle = dlopen(soPath.c_str(), RTLD_NOW | RTLD_NODELETE);
if (!handle) { /* log */ return; }

using CreateCmd = Cmd* (*)();
auto create = reinterpret_cast<CreateCmd>(dlsym(handle, symbol.c_str()));
if (!create) { dlclose(handle); return; }

Cmd* cmdObj = create();
cmdObj->SetCmd(cmd);
cmdObj->Init();
mapSo[cmd] = /* 保存 handle + cmdObj */;
```

### 3.3 插件导出示例（简化）

```cpp
class CmdGetToken final : public Cmd {
public:
    bool Init() override { return true; }
};

extern "C" Cmd* create() {
    return new CmdGetToken();
}
```

---

## 4. 反射化示例（统一协议）

### 4.1 公共接口头（host 与 plugin 共用）

```cpp
// PluginApi.hpp
struct CmdFactoryEntry {
    int cmd;
    Cmd* (*creator)();
};

struct PluginMeta {
    const char* pluginName;
    int version;
    int entryCount;
    const CmdFactoryEntry* entries;
};

extern "C" const PluginMeta* GetPluginMeta();
```

### 4.2 插件实现示例

```cpp
namespace {
Cmd* CreateCmd10001() { return new CmdGetToken(); }
Cmd* CreateCmd10002() { return new CmdRefreshToken(); }

const CmdFactoryEntry kEntries[] = {
    {10001, &CreateCmd10001},
    {10002, &CreateCmd10002},
};

const PluginMeta kMeta{
    "AuthPlugin",
    1,
    static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0])),
    kEntries
};
} // namespace

extern "C" const PluginMeta* GetPluginMeta() {
    return &kMeta;
}
```

### 4.3 Worker 加载示例（简化）

```cpp
void* handle = dlopen(soPath.c_str(), RTLD_NOW | RTLD_NODELETE);
if (!handle) return;

using GetMetaFn = const PluginMeta* (*)();
auto getMeta = reinterpret_cast<GetMetaFn>(dlsym(handle, "GetPluginMeta"));
if (!getMeta) { dlclose(handle); return; }

const PluginMeta* meta = getMeta();
for (int i = 0; i < meta->entryCount; ++i) {
    const auto& e = meta->entries[i];
    Cmd* obj = e.creator();
    obj->SetCmd(e.cmd);
    if (!obj->Init()) { delete obj; continue; }
    mapSo[e.cmd] = /* 保存 handle + obj */;
}
```

---

## 5. 优缺点对比

### 5.1 伪反射

优点：

- 简单直接，和现有代码一致
- 每个插件入口可独立定义，灵活
- 迁移成本最低

缺点：

- 配置项较多（`cmd` + `so_path` + `entrance_symbol`）
- 容易出现“配置与插件实现不一致”
- 多插件时维护成本上升

### 5.2 反射化

优点：

- 入口统一，加载器逻辑更干净
- 插件可自描述，配置更轻
- 对插件生态扩展友好（版本、能力、依赖可继续扩展到 meta）

缺点：

- 需要新增并维护一套插件协议
- 初期改造面大（host + plugins + 配置 + 回归）
- 若协议设计不稳，后续兼容成本会转移到协议层

---

## 6. 推荐落地路径（不冒险）

1. **第 1 步（低风险）**：保留现状，先约定默认 `entrance_symbol = create`，减少配置差异。  
2. **第 2 步（双栈）**：`Worker` 先尝试 `GetPluginMeta`，失败则回退旧逻辑。  
3. **第 3 步（迁移）**：新插件全部走 `GetPluginMeta`，老插件按计划逐步迁。  
4. **第 4 步（收敛）**：确认稳定后，再考虑移除旧入口读取逻辑。  

这样可以做到：**不停机思路迁移**，且每一步都可单独回滚。

