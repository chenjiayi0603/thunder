## 会话开头必读

每次新会话开始，**必须先读 `TEST_STATUS.md`**，了解当前测试状态和待做事项，避免重复测试：

```bash
cat TEST_STATUS.md
```

测试跑完后立即更新：

```bash
tests/save_status.sh          # 跑完整测试 + 更新状态
tests/save_status.sh --quick  # 只跑构建+ctest+pytest（跳过 E2E）
```

---

## deploytest — Thunder 本地部署测试

当用户说"deploytest"或"本地部署测试"时：

**唯一入口：`./deploy.sh`**

### 第一步：构建 + 单元测试
```bash
./deploy.sh test unit    # C++ gtest (~250 cases) + Python pytest (64 cases)，全部通过才继续
```

### 第二步：E2E 集成测试（Docker Compose）
```bash
./deploy.sh test e2e     # Docker compose up → 等待服务就绪 → pytest E2E → docker compose down
```
等价于手动流程：`./deploy.sh build` → `./deploy.sh up` → 等待端口 → `./deploy.sh test e2e --skip-build` → `./deploy.sh down`

> **deploytest 全程基于 Docker Compose**，不依赖 k8s，所有服务在容器内运行。

### E2E 覆盖范围

| 服务 | 测试内容 |
|------|---------|
| **Center (Raft)** | 节点选举、日志复制、服务注册/发现、集群状态查询、配置同步 |
| **HelloHttp** | GET/POST/PUT/DELETE 各方法、参数校验、错误处理 |
| **HelloHttps** | TLS 握手、证书校验、API 端点、异常断连 |
| **HelloWs** | WebSocket 连接→消息收发→断开、异常重连 |
| **Interface** | API 端点、参数校验、错误处理、插件加载→卸载 |
| **跨服务** | Manager-Worker 通信、心跳机制、全链路交互 |
| **Worker 优雅重启** | SIGTERM → 排空在途连接 → Manager 自动重启 Worker → 新 Worker 正常服务；验证：新 Worker 进程存在 + curl 正常 + 无 FATAL 日志 |
| **性能** | QPS、延迟 P99、内存占用 (真实 I/O) |

### 未覆盖项（待补充 E2E）

| 项目 | 当前状态 | 说明 |
|------|---------|------|
| **Lua SendToNodeType** | ❌ smoke 失败 | HELLO_HTTP 注册 etcd 问题待修（#113） |
| **SO 热更新** | ⚠️ 未验证 | build-so → extract → 服务加载新 SO 全链路 |
| **Lua 热更新** | ⚠️ 未验证 | Admin 下发新脚本 → Worker 重载 → 新逻辑生效（#110） |
| **etcd 节点注册完整性** | ⚠️ 未验证 | 确认所有预期 node_type 均出现在 etcd 注册表 |

### 第三步：Smoke 测试

```bash
# 先确认 etcd 注册节点完整
python3 tests/admin.py nodes
# 预期：HELLO_HTTP / HELLO_WS / HELLO_HTTPS / INTERFACE / LOGIC 均出现
# 若有缺失 → 停止，先排查注册问题，不得继续 smoke

# 再跑 smoke
tests/test_smoke.sh 2>&1 | tee /tmp/smoke_$(date +%Y%m%d_%H%M%S).txt
# 预期：0 失败；有任何失败 = 未通过
```

### 测试后清理
```bash
./deploy.sh clean        # 一键清理 build/ + Docker + tmp
```

### 规则
- 单元测试通过不算整体通过，E2E 必须也通过
- **E2E 通过不算 smoke 通过** — E2E 和 smoke 覆盖范围不同，必须分开跑、分开确认
- **smoke 有任何失败项 = 未通过** — 禁止只报总数（"15/18"），必须逐条列出失败项及原因
- **必须先确认所有预期服务节点已注册到 etcd，再开始 smoke 测试** — 若节点缺失，停止测试先排查注册问题，不得继续跑并把失败归咎于"超时"
- 失败则分析日志、修复、重试，最多 3 次
- 部分通过 = 未通过，要么全通要么明确列出未通过项及原因
- 模拟测试通过 ≠ 测试通过，硬件限制的标注"当前环境无法测试"及原因
- git add + commit + push 所有改动
---


---

## k8sregression — k8s 部署 + 全量回归测试

当用户说"k8sregression"或"k8s 回归测试"时：

### 第一步：构建 + 代码级测试

```bash
cd /home/tommychen/thunder/build
cmake --build . -j$(nproc)            # 全量构建，0 error
make install                           # 安装到 deploy 目录
ctest --test-dir code/test -j$(nproc)  # C++ gtest (331 cases)，99%+ 通过
python3 -m pytest tests/unit/ -q      # Python 单元测试 (133 cases)，全部通过
```

### 第二步：部署到 k8s

```bash
kubectl apply -f k8s/                 # 部署/更新所有服务
kubectl -n thunder rollout status deployment --timeout=120s  # 等待就绪
```

前置条件:
- k8s node 无 DiskPressure taint
- PV `thunder-plugins` 已就绪 (hostPath 或 NFS)
- NodePort: hello=30006, interface=30008, ws=30010, https=30043

### 第三步：端口转发 (NodePort 不可达时)

```bash
nohup python3 /tmp/k8s_fwd.py > /tmp/k8s_fwd.log 2>&1 &
```

### 第四步：配置测试并执行 E2E

```bash
cd /home/tommychen/thunder
# 指向 k8s NodePort
sed -i 's|127.0.0.1|192.168.3.61|g' tests/e2e/conftest.py
sed -i 's|https://127.0.0.1:27443|https://192.168.3.61:30043|' tests/e2e/test_https_hello.py
sed -i 's|27006|30006|g; s|27008|30008|g; s|27443|30043|g' tests/e2e/conftest.py
# 修复 sed 导致的 URL 断裂
sed -i 's|"http://192.168.3.61:|"http://|g' tests/e2e/conftest.py
sed -i '34s|"http://27008|"http://192.168.3.61:27008|' tests/e2e/conftest.py
sed -i '43s|"http://{p}|"http://192.168.3.61:{p}|g' tests/e2e/conftest.py

# 执行
python3 -m pytest tests/e2e/ -v --tb=line -m "integration or smoke" --mode=external

# 恢复
git checkout -- tests/e2e/conftest.py tests/e2e/test_https_hello.py
```

### E2E 覆盖范围

| 分组 | 用例数 | 预期 |
|------|:------:|------|
| HTTP hello | 4 | ✅ 全部通过 |
| HTTPS hello | 3 | ✅ SSL 证书正确时通过 |
| Interface chain | 5 | ✅ 4/5 (1 etcd 路由预存) |
| WS hello | 4 | ✅ 全部通过 |
| MultiCenter | 2 | ✅ 全部通过 |
| Stress | 1 | ✅ 通过 |
| WRK smoke | 2 | ✅ 通过 |
| etcd admin | 5 | ✅ 3 passed, 2 skipped (无注册数据+单节点) |
| **合计** | **21+** | **21/22 通过，1 预存失败** |

### 验收标准

- **构建**: 0 error
- **C++ gtest**: 99%+ (331 tests, 允许 SoDownload 预存失败)
- **Python unit**: 133/133 通过
- **E2E**: 20/21 通过 (允许 genkey_verifykey etcd 路由预存失败)
- 失败项标注原因 + 是否与本次改动相关

## 触发词：rearrange

当用户说 **rearrange** 时，执行以下流程：

### 适用场景
某个目录下有一堆内容重叠、未分类的 `.md` 文件，需要按功能重组。

### 核心原则
- **新文件 = 速查笔记风格**：精炼、结构化、方便面试前快速翻阅
- **有价值信息补回对应主题文件**：旧文件中的详细原理、完整示例、深入分析，不丢弃，直接补充到新文件对应章节中
- 宁可使单文件变大，也不丢失原理和例子

### 执行步骤

1. **读取所有文件**：读取目标目录下所有 `.md` 文件的内容（注意大文件分段读取）

2. **内容归类**：分析每份文件的主题和重叠点，设计功能分组方案

3. **去重合并 + 提取有价值信息**：
   - 同类内容合并，重复部分只保留最完整的一处
   - **同时将以下内容提取出来**，等新文件创建后补回：
     - 原理性长篇讲解（如"为什么这样设计"、"底层机制分析"）
     - 完整的可运行代码示例（非片段）
     - 对比分析（如 "A vs B 优缺点详解"）
     - 面试深挖中可能问到的扩展知识点
   - 新文件先只保留：核心结论 + 关键代码片段 + 对比表格 + 注意事项

4. **创建新文件**：
   - 创建 `00-总览.md` 作为索引总领文件（含文件地图、全景图、阅读路径）
   - 按功能创建 `01-*.md` 到 `N-*.md`，每份文件自成体系（核心原理 + 关键代码 + 注意事项）
   - 面试考点汇总到最后一篇

5. **将提取的有价值信息补回对应文件**：
   - 原理说明 → 补到对应主题文件的对应章节下
   - 完整示例 → 补到对应主题文件的代码示例区
   - 扩展知识点 → 补到对应文件的「深入理解」或「常见陷阱」章节
   - 确保新文件内容充实，不依赖外部文档

6. **旧文件清理**：确认新文件写完后，删除所有原始旧文件

7. **更新 CLAUDE.md 目录结构**：将新的目录结构反映到本文档的仓库目录结构中

### 文件命名规则

重组后的文件使用 `{序号}-{技术栈前缀}-{主题}.md` 格式：

```
02-go-并发编程.md    # go 技术栈
01-cpp-C++基础语法.md # cpp 技术栈
```

- **技术栈前缀**：当目录名称不能直接体现技术归属时（如 `go/` 目录下的文件在文件浏览器中可能脱离目录上下文），在序号后加技术栈前缀（如 `go`、`cpp`）
- **不需要前缀**：如果目录名本身就是技术名（如 `cpp/`），且文件在目录内引用无歧义，可省略前缀
- **一致性**：同一目录下所有文件保持统一的命名风格

### 要点列举必须带示例

列出多个技术要点时（如「六种逃逸场景」「五种实现方式」等），**每个要点必须附带独立代码示例**，不能用一行注释笼统带过。

❌ 反例（只有名词，无代码）：
```markdown
**六种逃逸场景**：返回指针、interface 调用、闭包、channel 发指针、大对象、切片扩容。
```

✅ 正例（逐条展开，每项有独立代码）：
```markdown
**六种逃逸场景**（含示例）：

```go
// 1. 返回指针
func escape1() *int {
    x := 42
    return &x  // x 逃逸到堆
}

// 2. interface 调用
func escape2() {
    x := 42
    fmt.Println(x)  // x 逃逸（fmt 参数为 interface{}）
}
// ... 其余逐条列出
```
```

**例外**：纯名词罗列（如文件列表、目录结构）不需要逐条代码。

### 禁止「其他」兜底分类

重构或增强文件时，**禁止**出现笼统的兜底章节（如 `### X.Y 其他重要特性` / `### X.Y 其他实用特性`），必须将杂项逐条拆解为**独立子节**（`#### X.Y.Z 具体名称`），每节包含：

| 要素 | 说明 |
|:----|:-----|
| **解决的问题** | 为什么需要这个特性/概念，解决了什么痛点 |
| **完整代码示例** | 含输入/输出/正反对比的可工作代码 |
| **性能/注意事项** | 零开销保证、常见陷阱、选型建议 |

❌ 反例（笼统堆砌）：
```markdown
### 1.5 其他重要特性（含示例）
```cpp
// nullptr —— 类型安全空指针
// enum class —— 强类型枚举
// constexpr —— 编译期计算
```
```

✅ 正例（逐条展开）：
```markdown
### 1.5 类型安全与枚举增强

#### 1.5.1 nullptr — 类型安全空指针

**解决的问题**：`NULL` 本质是整数 `0`，重载解析中会意外匹配 `int` 版本。

```cpp
void foo(int);  void foo(char*);
foo(NULL);      // 调用 foo(int) —— 危险！
foo(nullptr);   // 调用 foo(char*) —— 正确
```

**性能**：零开销抽象，运行时就是 `0`。

---
```

---

### 触发词：rearrange docs
- docs 目录编号前缀扁平化(如 `01-xxx.md`, `02-xxx.md`), 无子目录
- 参考模板: english-learner/docs/architecture/
### 触发词：logs / 日志
- 用 tests/logs.sh 查看, 支持 --logic/--interface/--etcd 指定节点
### 触发词：smoke / 冒烟
- tests/test_smoke.sh --hello/--interface/--etcd 分段测试
### 触发词：chaos / 混沌
- tests/chaos_etcd.sh 三个场景(停服/重启/灾难)

### 触发词：issus / 问题清单
- 所有 bug/优化/设计问题统一记录在 `issus-list.md`
- 发现新问题 → 先确认是真实问题 → 再记录 → 再修复
- 修复后标记 `✅ 已修复`,未修复标记 `🟡`
- 不要未经确认就改状态,不要删除已有条目
### 触发词：代码移动
- `git mv` 移动文件,同步修正所有 include 和 CMakeLists
- 全量构建 + 冒烟验证无回归
### 触发词：删代码
- 先确认零引用 → `grep -rn` 全局搜索 → 再删
- 测试文件如引用也一并清理

### 触发词：性能测试
- 没测就是没测, 别填假数据
- 对比测试要保证只有一个变量不同 (如 body 大小变化、其他条件一致)
- 每次改 backend 配置后等 5 秒让服务重启完成
- 结果直接写入对应文档, 别存脑子里

### 🚫 禁止回退
- **禁止 git reset/rebase 丢弃代码** — 除非用户明确要求
- **禁止 git checkout 覆盖修改** — 所有文件变动必须经过确认
- **禁止 rebase skip** — 冲突时合并解决, 不跳过有效提交
- **禁止 revert file moves/refactors** — 原因: 上次 rebase skip 导致 io/ register/ 目录丢失

### 提交规范 (Commit Rules)
- **只能 git merge，禁止 git rebase** — rebase 会改写历史, 丢弃本地提交
- **有冲突必须手动解决** — 不允许 --skip / --abort / --force
- **解决冲突后立即验证** — 全量编译 + 冒烟测试
- **每步提交前确认工作树干净** — git status 检查无遗漏


### testnewfunc 触发词 (Thunder)

当用户说"testnewfunc"时，执行以下流程：

**1. 定位改动范围**
```bash
git diff HEAD --stat          # 未提交更改
git log --oneline -3           # 最近提交
```
确定影响范围：code/Net | code/Hello* | deploy/admin-web | k8s | build

**2. 全量构建**
```bash
./deploy.sh build              # cmake + make + install, 必须 0 error 0 warning
```

**3. C++ 单元测试**
```bash
ctest -j4 --output-on-failure  # 从 build/code/test 目录运行
```
- 328 项必须 100% 通过
- 失败项逐一排查，不允许跳过

**4. Python 单元测试**
```bash
cd tests && python -m pytest pytest/ -v
```

**5. k8s 部署**（涉及 k8s 配置或部署文件时）
```bash
kubectl apply -f k8s/
kubectl -n thunder rollout restart deployment thunder-admin-web
```

**6. Admin 功能测试**（涉及 Admin 页面改动时）
- 页面可访问: `curl http://127.0.0.1:30090/index.html` → HTTP 200
- SO 镜像列表: `curl http://127.0.0.1:30090/api/so-images` → 返回 JSON
- SO 文件列表: `curl http://IP:8090/api/so-files?image=xxx` → 返回 .so 列表
- SO 提取: `curl -X POST http://IP:8090/api/so-extract ...` → 本地+NFS 双写验证
- 页面功能: grep 检查 selectSoImage / extractAndRefresh / triggerUpdate 等函数存在
- **必须真实请求，禁止 mock**

**7. SO 镜像构建**（涉及 so-images 或 deploy.sh 改动时）
```bash
./deploy.sh build-so all        # 全量构建
./deploy.sh build-so HelloHttp_ModuleHello  # 单独构建
```
- 首次构建 → 全量通过
- 二次构建 → 全部跳过(无变化)

**8. 回归测试（影响范围内的旧功能）**
- 分析改动影响范围，列出受影响的旧功能
- 跑受影响的相关测试
- 不跑全量回归（除非用户明确要求）

**9. 端到端测试（新增/修改的功能）**
- 针对本次改动的功能点，明确列出测试场景
- 实际跑通完整链路，展示运行输出
- 跑不通就说明具体卡在哪，不要跳过

**测试输出要求**:
- 每个测试项必须展示：命令 + 完整输出 + 结果
- 通过 ✅ / 失败 ❌ / 跳过 ⏭ 必须明确标注
- 部分通过 = 未通过，必须列出原因
- 构建失败、ctest 失败 = 阻塞，先修复再继续

**禁止的测试方式**:
- ❌ 只跑 ctest 就说"测试通过"
- ❌ curl 健康检查就说"功能正常"
- ❌ 改完代码不跑测试就提交
- ❌ 说"已验证"但不展示完整输出
- ❌ 部分通过就说"测试通过"
- ❌ E2E 通过就说"smoke 也通过"（两者覆盖范围不同，必须分别跑）
- ❌ smoke 有失败项却汇报"全部通过"或只报通过数不报失败数
- ❌ 节点未注册到 etcd 就开始跑 smoke，把路由超时当"预期失败"忽略

## Agent 行为准则

### 1. 先思考再编码（Think Before Coding）
- 不确定时必须停下来问，不能猜，不能假设
- 存在多种理解时列出选项让用户选，不要替用户做决定
- 发现更简单的方案时主动说出来，不要默默选最复杂的路
- 把 trade-off 摆出来，不要隐藏困惑

### 2. 简洁优先（Simplicity First）
- 50 行能写完绝不写 200 行
- 没人要求的"灵活性"和"可配置"不加
- 不可能发生的异常场景不做错误处理
- 不为未来可能的需求提前写代码

### 3. 精准修改（Surgical Changes）
- 只动被要求动的部分，不顺手优化相邻代码
- 匹配项目已有的代码风格，哪怕觉得自己写得更好
- 看到不相关的问题提一嘴就行，别动手改
- 每一行改动都能追溯到用户的原始请求

### 4. 禁止擅自提交（No Unauthorized Commits）
- **除非用户明确说"提交"、"commit"、"push"、"推"，否则绝不执行 git commit / git push**
- 改完代码 → 测试 → 汇报结果 → **停**，等用户指示
- 即使改了一堆文件、测试全绿，也不能自己决定提交
- 这条优先级高于其他所有行为准则

### 5. 目标驱动执行（Goal-Driven Execution）
- "修 Bug" → 先写能复现 Bug 的测试，再让测试通过
- "加校验" → 先写非法输入测试，再让它通过
- "重构 X" → 确保改前改后测试都通过
- 复杂任务先列分步计划，每步带验证方式

