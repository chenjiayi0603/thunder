# simdjson vs yyjson vs rapidjson — Thunder JSON 库迁移分析

> 背景：Thunder 当前使用 `CJsonObject`（封装 cJSON 2009），482 处引用，需评估替换方案。
> 详见 issus-list.md #81。

---

## 0. 库基本信息对比（实测数据，截至 2026-06-13）

| 指标 | **simdjson** | **yyjson** | **rapidjson** | **YAJL** | **nlohmann/json** | **cJSON（当前）** |
|------|-------------|-----------|--------------|---------|------------------|-----------------|
| **GitHub** | [simdjson/simdjson](https://github.com/simdjson/simdjson) | [ibireme/yyjson](https://github.com/ibireme/yyjson) | [Tencent/rapidjson](https://github.com/Tencent/rapidjson) | [lloyd/yajl](https://github.com/lloyd/yajl) | [nlohmann/json](https://github.com/nlohmann/json) | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON) |
| **GitHub Stars** | ⭐ 23.8k | ⭐ 3.8k | ⭐ 15.1k | ⭐ 2.2k | ⭐ 49.9k | ⭐ 12.8k |
| **最新版本** | v4.6.4 | v0.12.0 | v1.1.0 | v2.1.0 | v3.12.0 | v1.7.19 |
| **最新版发布时间** | 2026-05-06 | 2025-08 | **2016-08-25** | **无 GitHub Release** | 2026 | 2025-09-09 |
| **最后 commit** | 2026-05 | 2025-08 | 2025-02 | **2015-09** | 2026 | 2025-09 |
| **首次发布时间** | 2019-03 | 2020-10 | 2015-04 | 2011 | 2013 | 2009 |
| **总发布次数** | 110 次 | 12 次 | **5 次** | **0 次（无 Release）** | 80+ 次 | 49 次 |
| **发布频率** | ~15 次/年（活跃） | ~2 次/年（稳定） | **0 次/年（停滞）** | **0 次/年（停滞）** | ~6 次/年（活跃） | ~3 次/年（缓慢）|
| **Open Issues** | 124 | 30 | **673** | 86 | 26 | 221 |
| **语言** | C++17 | C11 | C++11 | C89 | C++11 | C89/ANSI C |
| **依赖** | 零（自含） | 零（单文件）| 零（header-only）| 零 | 零（单头文件）| 零（单文件）|
| **License** | Apache 2.0 | MIT | MIT | ISC | MIT | MIT |
| **维护状态** | 🟢 非常活跃 | 🟢 活跃 | 🔴 **停止（2016）** | 🔴 **停止（2015）** | 🟢 非常活跃 | 🟡 缓慢维护 |
| **流式输入（SAX）** | ❌ | ❌ | ✅ FileReadStream | ✅ **回调驱动** | ❌ | ❌ |
| **作者** | Daniel Lemire（加拿大 UQAM 教授）| ibireme（郭耀源，独立开发者）| miloyip（腾讯游戏工程师）| Lloyd Hilaiel（Mozilla）| Niels Lohmann（德国开发者）| Dave Gamble |
| **主要用户** | Node.js, ClickHouse, Meta Velox | **DuckDB**, Zrythm, Python/Swift/Go 多个绑定 | 腾讯（原作者）| 嵌入式/IoT 项目 | 广泛（学术/工业）| 嵌入式/IOT |

> **yyjson 稳定性说明**：作者 ibireme（郭耀源）是中国独立开发者，YYKit / YYText / YYWebImage 系列 iOS 库作者，GitHub 12.2k followers。yyjson 当前版本 v0.12.0，尚未到 v1.0，理论上 API 在 1.0 前可以变动，**但实际生产用户包括 DuckDB（工业级嵌入式数据库）**，API 已相当稳定。CI 覆盖 gcc/clang/msvc/tcc 多编译器 + valgrind + sanitizer + libfuzzer，质量有保证。对 Thunder 而言风险可控——即使 API 调整，影响范围也只在 `CJsonObject.cpp` 一个文件内。

> **rapidjson 停止维护说明**：最后一次正式发布为 2016 年 v1.1.0，此后 9 年无新版本，673 个 open issues 无人处理。2025-02 有一次安全 bugfix commit，但项目已无实质开发。**仍纳入对比是因为其 SAX 接口支持流式输入。**

> **YAJL 停止维护说明**：作者 Lloyd Hilaiel（Mozilla 工程师），最后 commit 为 **2015-09**，比 rapidjson 还早。无 GitHub Release，86 个 open issues。**纳入对比的唯一原因是其 SAX 回调模型是 C89 里最干净的流式解析接口**，但已停止维护 11 年，生产使用需自行承担维护风险。流式 JSON 在 C/C++ 中**没有维护中的主流方案**。

### 版本号含义

| 库 | 版本号策略 | 说明 |
|----|-----------|------|
| simdjson | `4.x.y` 语义化 | 主版本升级表示 API 破坏性变更，当前主版本 4 已稳定 |
| yyjson | `0.x.y` | 主版本仍为 0，作者明确表示 API 在 1.0 前可能变动；实际 API 已较稳定 |
| rapidjson | `1.1.x` | 2016 年停在 v1.1.0，API 冻结 |
| nlohmann | `3.x.y` | 主版本 3 自 2018 年保持，极稳定 |
| YAJL | `2.x.y` | 停在 v2.1.0（2012），无正式 Release，API 冻结 |
| cJSON | `1.7.x` | 多年停在 1.7，修复为主 |

---

## 1. 快速结论

| 维度 | simdjson | yyjson | rapidjson |
|------|---------|--------|----------|
| **解析速度** | ⚡ 极快，大文档领先 | ⚡ 极快，小文档略优 | 中等（~0.8 GB/s）|
| **写 JSON** | ❌ 不支持 | ✅ 完整支持 | ✅ Writer/PrettyWriter |
| **按需惰性解析** | ✅ On-Demand（不构建完整 DOM）| ❌ 全量解析为 DOM | ❌ 全量 |
| **流式输入（边收边解析）** | ❌ 需完整文档在内存 | ❌ 需完整文档在内存 | ✅ **SAX + FileReadStream** |
| **流式写出** | ❌ 无 writer | ⚠️ 可增量构建 | ✅ Writer + OStreamWrapper |
| **替换 CJsonObject** | ❌ 只能替换解析半边 | ✅ 解析+写出全替换 | ✅ 可全替换，但 API 复杂 |
| **可变文档** | ❌ 只读 | ✅ `yyjson_mut_doc` | ✅ `Document` 可变 |
| **API 风格** | C++17，range-for | C11，低层指针 | C++11，模板重，繁琐 |
| **引入成本** | 中（双文件）| 低（单 C 文件）| 低（header-only）|
| **内存占用** | On-Demand 极低 | Pool allocator | 自定义 allocator |
| **错误处理** | `simdjson_result<T>` | 返回 NULL | 返回 bool/错误码 |
| **维护状态** | 🟢 非常活跃 | 🟢 活跃 | 🔴 **停止维护（2016）** |

**Thunder 场景推荐**：

- **替换 CJsonObject** → **yyjson**（Parse+Write+Mutable，引入成本最低）
- **大 payload 按需解析** → **simdjson On-Demand**（与 yyjson 并存）
- **真正流式输入（边收边解析）** → **rapidjson SAX**，但需接受其已停止维护的风险

---

## 2. 详细对比

### 2.1 性能

#### 实测数据（本机，2026-06-13）

**环境**：Intel i9-12900H，-O3 -march=native，C++20

```
小 JSON（141 字节，Thunder HTTP handler 典型响应，50 万次）
按 parse MB/s 排序（高 → 低），↑ 表示快于 cJSON 基准

┌──────────────────────┬───────────────────┬──────────┬───────────────────┬──────────┬─────────────────┐
│         库           │   parse  MB/s     │   ns/op  │   build  MB/s     │   ns/op  │      备注       │
├──────────────────────┼───────────────────┼──────────┼───────────────────┼──────────┼─────────────────┤
│ simdjson (On-Demand) │ 3726  ( 7.9×↑)   │    37.8  │       N/A         │    N/A   │ 无 writer       │
│ yyjson               │ 1944  ( 4.1×↑)   │    72.5  │ 1567  (11.7×↑)   │   90.0   │ 最快 build ★   │
│ simdjson (DOM)       │ 1894  ( 4.0×↑)   │    74.5  │       N/A         │    N/A   │ 无 writer       │
│ cJSON（当前）        │  471  [基准 1×]   │   299.4  │  134  [基准 1×]   │ 1056.0   │                 │
│ rapidjson            │  413  ( 0.9× )   │   341.5  │  462  ( 3.5×↑)   │  305.3   │                 │
│ YAJL (SAX/流式)      │  410  ( 0.9× )   │   343.8  │  239  ( 1.8×↑)   │  589.4   │ 唯一流式输入    │
└──────────────────────┴───────────────────┴──────────┴───────────────────┴──────────┴─────────────────┘

中 JSON（583 字节，配置下发典型结构，10 万次）
按 parse MB/s 排序（高 → 低）

┌──────────────────────┬───────────────────┬──────────┬───────────────────┬──────────┬─────────────────┐
│         库           │   parse  MB/s     │   ns/op  │   build  MB/s     │   ns/op  │      备注       │
├──────────────────────┼───────────────────┼──────────┼───────────────────┼──────────┼─────────────────┤
│ simdjson (On-Demand) │ 5615  (20.8×↑)   │   103.8  │       N/A         │    N/A   │ 无 writer       │
│ simdjson (DOM)       │ 2321  ( 8.6×↑)   │   251.2  │       N/A         │    N/A   │ 无 writer       │
│ yyjson               │ 1994  ( 7.4×↑)   │   292.4  │ 6258  (11.1×↑)   │   93.2   │ 最快 build ★   │
│ YAJL (SAX/流式)      │  499  ( 1.8×↑)   │  1167.7  │ 1005  ( 1.8×↑)   │  580.0   │ 唯一流式输入    │
│ rapidjson            │  459  ( 1.7×↑)   │  1271.3  │ 1863  ( 3.3×↑)   │  312.9   │                 │
│ cJSON（当前）        │  270  [基准 1×]   │  2158.6  │  564  [基准 1×]   │ 1033.1   │                 │
└──────────────────────┴───────────────────┴──────────┴───────────────────┴──────────┴─────────────────┘

注：YAJL parse = SAX 回调（流式输入），simdjson build = N/A（无 writer）
环境：i9-12900H，-O3 -march=native，热身 2 轮，2026-06-13
```

#### 关键结论

- **simdjson On-Demand parse**：最快，领先 yyjson 约 2×，领先 cJSON 约 8–21×
- **yyjson build+serialize**：最快写出，比 cJSON 快 **11×**，比 YAJL 快 **6–7×**
- **YAJL parse(SAX)**：比 cJSON parse 还慢（每个 token 触发一次回调开销），唯一价值是**支持流式输入**
- **rapidjson**：parse/build 全面落后，综合最差
- **cJSON（当前）**：build 是最慢的，是 yyjson 的 1/11

### 2.2 流式 JSON —— 核心差异

#### simdjson On-Demand（按需惰性解析，非流式输入）

> **重要区分**：On-Demand 是"流式读取"不是"流式输入"。
> 完整 JSON 必须已在内存，解析器不分块接收；节省的是 CPU 和中间 DOM 内存，不是 IO 等待。

On-Demand 是 simdjson 的杀手锏：解析器不构建完整 DOM，只在调用时才扫描下一个 token。

```cpp
// simdjson On-Demand：惰性迭代，内存接近零（只要 padded_string 缓冲区）
padded_string json = padded_string::load("big.json");
ondemand::parser parser;
ondemand::document doc = parser.iterate(json);

// 只访问需要的字段，不读的字段零开销
for (ondemand::object item : doc["results"]) {
    std::string_view id   = item["id"];   // 只在这里才实际解析
    std::string_view name = item["name"];
    // 其他字段完全跳过
}
```

关键特性：
- **一次线性扫描**，不回头。适合流式处理大数组
- 字段顺序依赖：必须按 JSON 文档中的出现顺序访问（否则报错）
- **无法随机访问**，无法回溯

适合 Thunder 的场景：
- 解析 etcd 大型配置响应（只取几个字段）
- 解析大 JSON 数组响应，逐条处理

#### rapidjson SAX（唯一支持真正流式输入的候选）

rapidjson 的 SAX 接口是三者中**唯一**能边接收 bytes 边解析的方案：

```cpp
// 事件驱动，边读边回调，全程无需完整文档在内存
struct MyHandler : public rapidjson::BaseReaderHandler<> {
    bool Key(const char* str, rapidjson::SizeType, bool) {
        currentKey_ = str;
        return true;
    }
    bool String(const char* str, rapidjson::SizeType, bool) {
        results_[currentKey_] = str;
        return true;
    }
    std::string currentKey_;
    std::map<std::string, std::string> results_;
};

// 从文件流（或网络 buffer）逐块读取，真正流式
FILE* fp = fopen("big.json", "r");
char buf[65536];
rapidjson::FileReadStream is(fp, buf, sizeof(buf));  // 64KB 滑动窗口
MyHandler handler;
rapidjson::Reader reader;
reader.Parse(is, handler);  // 边读文件边触发回调，内存始终只有 64KB
```

适合场景：
- 超大 JSON 文件（GB 级），内存不够放完整文档
- 网络分块接收，一边收一边处理

**缺点**：
- 项目停止维护（2016），673 个 open issues 积压
- SAX 编程模型繁琐，需手写状态机管理解析上下文
- DOM 部分内存模型特殊，替换 `CJsonObject` 需要较多适配

#### yyjson（无流式解析，有增量写出）

yyjson 要求完整文档在内存，不支持 chunked/streaming 解析：

```c
// 必须完整文档
yyjson_doc *doc = yyjson_read(str, len, 0);  // 全量解析
yyjson_val *root = yyjson_doc_get_root(doc);
yyjson_val *name = yyjson_obj_get(root, "name");
const char *val = yyjson_get_str(name);
yyjson_doc_free(doc);
```

增量 **写出**（构建 JSON，非解析）：

```c
// 可以逐字段构建，最后一次性序列化
yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
yyjson_mut_val *root = yyjson_mut_obj(doc);
yyjson_mut_doc_set_root(doc, root);

yyjson_mut_obj_add_str(doc, root, "option", "TestHello");
yyjson_mut_obj_add_int(doc, root, "code", 0);

// 序列化时一次性输出，无真正流式
const char *json_str = yyjson_mut_write(doc, 0, NULL);
yyjson_mut_doc_free(doc);
free((void*)json_str);
```

#### 真正的流式写出：两者均不支持

如果需要"边生成边发送"（不在内存中缓冲完整 JSON 字符串），两者都做不到。
这类场景应用 `yyjson_write_fp`（写文件）或自己实现分块序列化。

### 2.3 可变文档（写 JSON）

这是 Thunder 的关键需求。`CJsonObject` 同时做 Parse + Add + ToString。

```cpp
// CJsonObject 用法（Parse + Write 同一对象）
util::CJsonObject j;
j.Add("code", 0);
j.Add("msg", "ok");
j.Add("data", subObj);
step.ResponseToClient(200, j.ToString());
```

**simdjson**：不支持写。simdjson 只能解析，没有 JSON writer。需要配合第三方 writer（如 nlohmann 的 dump，或手写字符串拼接）。

**yyjson**：`yyjson_mut_doc` 完整支持：

```c
yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
yyjson_mut_val *root = yyjson_mut_obj(doc);
yyjson_mut_doc_set_root(doc, root);
yyjson_mut_obj_add_int(doc, root, "code", 0);
yyjson_mut_obj_add_str(doc, root, "msg", "ok");

size_t len;
char *str = yyjson_mut_write(doc, 0, &len);
std::string result(str, len);
free(str);
yyjson_mut_doc_free(doc);
```

### 2.4 API 风格

**simdjson（现代 C++17）**：

```cpp
#include "simdjson.h"
using namespace simdjson;

ondemand::parser parser;
padded_string json = R"({"key":"value","num":42})"_padded;
ondemand::document doc = parser.iterate(json);

std::string_view key = doc["key"];   // string_view，零拷贝
int64_t num          = doc["num"];   // 类型安全，result<T>
```

- 结构化绑定、range-for、`std::string_view`
- `simdjson_result<T>` 强制错误处理（类似 Rust Result）
- 需要 C++17，Thunder 已用 C++20，完全兼容

**yyjson（C11 API）**：

```c
yyjson_doc *doc = yyjson_read(str, len, 0);
if (!doc) { /* 解析失败 */ }

yyjson_val *root = yyjson_doc_get_root(doc);
yyjson_val *key  = yyjson_obj_get(root, "key");
const char *val  = yyjson_get_str(key);  // NULL if not string

yyjson_doc_free(doc);
```

- 返回 NULL 代表错误，需手动判空
- 无 C++ 包装层，直接操作指针
- 需要为 Thunder 写 `CJsonObject` 的兼容封装层

### 2.5 内存管理

**simdjson**：
- DOM：O(n) 内存，比 cJSON 节省（紧凑 tape 格式）
- On-Demand：几乎零额外内存（只需 padded 输入缓冲 + 32 字节解析器状态）
- `padded_string` 要求输入末尾有 SIMDJSON_PADDING（64字节）填充——需要复制字符串

**yyjson**：
- 内置 pool allocator，内存碎片极少
- 比 cJSON 内存效率高约 2–3x
- `yyjson_mut_doc` 使用独立 pool，与 immutable doc 分离

### 2.6 引入成本

**simdjson**：

```cmake
# 两个文件（amalgamated）
FetchContent_Declare(simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson
  GIT_TAG v3.9.4)
FetchContent_MakeAvailable(simdjson)
target_link_libraries(Net PRIVATE simdjson)
```

- 需要 C++17（Thunder 已满足）
- 不支持 `noexcept` 全局（部分函数可能抛异常）
- 编译时间比 yyjson 长（模板较多）

**yyjson**：

```cmake
# 单个 C 文件，极简
add_library(yyjson STATIC yyjson.c)
target_include_directories(yyjson PUBLIC .)
target_link_libraries(Util PRIVATE yyjson)
```

- 仅 yyjson.h + yyjson.c，零依赖
- 纯 C11，编译极快
- `CJsonObject.cpp` 内部替换，外部接口零感知

---

## 3. Thunder 具体场景映射

| Thunder 场景 | 当前 | 推荐替换 |
|-------------|------|---------|
| HTTP handler 构建响应 JSON | `CJsonObject::Add` + `ToString` | yyjson `mut_doc` |
| 解析 HTTP 请求 body | `CJsonObject::Parse` | yyjson `read` |
| 解析 etcd 配置（大 payload）| `CJsonObject::Parse` | simdjson On-Demand |
| 解析 admin 下发配置 | `CJsonObject::Parse` | yyjson（文档小，兼容性优先）|
| Log 结构化输出 | `CJsonObject::Add` | yyjson `mut_doc` |
| Worker IPC 消息体 | `CJsonObject` | yyjson（大量小 JSON，延迟敏感）|

---

## 4. 迁移方案

### 方案 A：yyjson 替换底层（推荐，风险最低）

仅改 `CJsonObject.cpp` 内部实现，从 cJSON 换成 yyjson，保持 `CJsonObject` 接口不变。

```
影响范围：CJsonObject.cpp（1 个文件）
接口变动：零
回归范围：ctest 342 cases
预估工时：2–3 天（含测试）
风险：低
```

### 方案 B：新场景引入 simdjson（增量，零风险）

不动 `CJsonObject`，只在新增的大 payload 解析场景用 simdjson On-Demand。

```
影响范围：新增场景（如 etcd 大响应解析）
接口变动：新 API
回归范围：新增测试
预估工时：1 天
风险：极低
```

### 方案 C：方案 A + B 组合（最终形态）

```
Phase 1: yyjson 替换 CJsonObject 底层（解析+写出统一）
Phase 2: etcd/大 payload 场景引入 simdjson On-Demand（流式读）
Phase 3: 长期考虑是否完全废弃 CJsonObject 改为直接 API
```

---

## 5. 不选 simdjson 作为 CJsonObject 底层的原因

一句话：**`CJsonObject` 要同时做 Parse 和 Write，simdjson 只能 Parse**。

### 5.1 没有 Writer（最根本）

`CJsonObject` 的核心用法是 `Add` + `ToString`，482 处引用里有一半在构建 JSON 响应：

```cpp
util::CJsonObject j;
j.Add("code", 0);
j.Add("msg", "ok");
step.ResponseToClient(200, j.ToString());
```

simdjson 没有任何 JSON 生成 API。替换后写出半边要另找第二个库，变成两个库拼凑，引入成本翻倍，还不如不换。

### 5.2 DOM 只读，无法修改文档

`CJsonObject::Replace`、`Delete`、`operator[]` 都要求可变文档。simdjson 解析出来的 DOM 是只读的，On-Demand 更是一次性前向迭代，没有可变文档的概念，这些操作根本无法适配。

### 5.3 On-Demand 字段顺序依赖

On-Demand 必须按 JSON 里的物理顺序访问字段，乱序访问直接报错：

```cpp
// simdjson On-Demand：字段必须按文档顺序访问
for (auto field : doc.get_object()) { ... }  // 只能顺序迭代

// CJsonObject：随机键查找，顺序无关
obj.Get("z_field", val);  // 不管 z_field 在文档哪个位置都能取到
obj.Get("a_field", val);
```

Thunder 的 `Get("key")` 语义是随机键查找，与 On-Demand 完全不兼容，无法做透明替换。

### 5.4 padded_string 要求额外内存拷贝

simdjson 要求输入字符串末尾有 64 字节 padding（`padded_string`）。Thunder 里的 JSON 来源是网络 buffer、etcd 响应、配置字符串，都不带 padding，每次解析前必须额外 copy 一次。

Worker IPC、HTTP handler 这类高频小 JSON 场景，这个强制拷贝的开销反而比 cJSON 还高。

### 5.5 C++17 重模板，引入 Util 层不干净

`CJsonObject` 在 Util 层，面向 C 和 C++ 消费者。simdjson 是重度 C++17 模板库，引入 Util 会显著拉高编译成本，且与 Util 目前 C11/C++11 兼容目标不一致。

yyjson 是单个 `.c` 文件，drop-in 到 Util 零副作用，不污染任何依赖链。

### 小结

| 原因 | simdjson | yyjson |
|------|---------|--------|
| 能替换 Write 半边 | ❌ 无 writer | ✅ `yyjson_mut_doc` |
| 可变文档 | ❌ 只读 | ✅ `yyjson_mut_*` |
| 随机键查找 | ❌ On-Demand 顺序依赖 | ✅ `yyjson_obj_get` |
| 零拷贝接入 | ❌ 需 padded_string | ✅ 直接传指针+长度 |
| Util 层引入成本 | ❌ C++17 重模板 | ✅ 单 C 文件 |

simdjson 的价值在"大 payload 按需惰性解析"（不构建完整 DOM，节省 CPU 和内存），这是 Thunder **目前没有的瓶颈场景**。等 etcd 大响应解析成为瓶颈时，再以独立模块的方式引入 simdjson On-Demand，与 yyjson 并存，不冲突。
3. **On-Demand 顺序依赖**：字段必须按文档顺序访问，`CJsonObject` 的随机 `Get(key)` 语义无法满足。
4. **padded_string 开销**：每次解析都需要 copy 加 padding，Thunder 的高频小 JSON 场景有额外内存分配。

---

## 6. 验收标准（方案 A）

- [ ] `CJsonObject` 所有公开 API 行为与替换前一致（单元测试覆盖）
- [ ] ctest 342/342 全部通过
- [ ] 10MB JSON 解析 < 10ms（比 cJSON 提升 ≥ 5x）
- [ ] 内存峰值不超过当前 cJSON 实现的 1.5x
