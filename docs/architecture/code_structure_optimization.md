
# Thunder 代码结构优化报告 — 死代码与冗余分析

> 日期: 2026-06-01 | 基于完整代码扫描

---

## 一、已清理 (本次发现并删除)

| 项目 | 数量 | 大小 | 说明 |
|------|------|------|------|
| 旧日志文件 | ~30 个 | **2.6 GB** | deploy/Center/log, Interface/log, Logic/log 里的轮转日志 |
| 冗余二进制 Hello | 3 个 | **129 MB** | HelloWs/bin/Hello, HelloHttps/bin/Hello, HelloHttp/bin/Hello — CMake 只部署 HelloXxx, Hello 是旧产物 |
| .bak 备份文件 | 3 个 | ~200 KB | Hello.json.bak, Hello.json.bak_ev, oss_sys.pb.cc.bak |
| 重复配置 | 1 个 | 1 KB | Hello_test_ev.json == Hello_ev.json 完全相同 |
| **本次清理合计** | | **~2.7 GB** | |

---

## 二、架构级冗余 (需要代码改造)

### 🔴 P0: 全量编译 ALL sources → 每个二进制包含死代码

**根因**: `code/Net/CMakeLists.txt` 第11行:
```cmake
file(GLOB_RECURSE NET_ALL_SOURCES CONFIGURE_DEPENDS
  "${NET_SRC}/*.cpp"
  "${NET_SRC}/*.cc"
)
add_executable(Hello ${NET_ALL_SOURCES})    # ← 全部 .cpp 都编译进去!
```

**后果**: 每个 Hello/Center/Logic/Interface 二进制都包含了:
```
AsioUringIoBackend.cpp  (567行)  ← THUNDER_IO_ASIO_URING=OFF, 死代码!
DpdkIoBackend.cpp       (179行)  ← 无 DPDK 硬件, 死代码!
NativeUringIoBackend.cpp(424行)  ← 运行时用 ev, 未使用!
Worker.cpp               (6020行) ← Logic/Interface 二进制也包含完整 Worker
Manager.cpp              (2732行) ← Hello 二进制也包含完整 Manager
```

**量化**: 每个 43MB 的二进制中, 保守估计 15-25% 是未使用代码
→ 5个服务 × 43MB × 20% = **~43MB 死代码 (运行时占用内存)**

### 🔴 P1: 6 个服务 = 1 个可执行文件 (5 份字节级拷贝)

**CMakeLists.txt 第41-72行**:
```cmake
thunder_deploy_copy(Hello "HelloHttp/bin/HelloHttp")    # ← 原始
# ...
COMMAND copy_if_different Hello → HelloWs/bin/HelloWs     # ← 拷贝1
COMMAND copy_if_different Hello → HelloHttps/bin/HelloHttps # ← 拷贝2
COMMAND copy_if_different Hello → Logic/bin/Logic          # ← 拷贝3
COMMAND copy_if_different Hello → Interface/bin/Interface  # ← 拷贝4
```

**5个服务二进制 = 1个 Hello 目标, 5份拷贝, 每份 43MB**
Center 有 `NODE_TYPE_CENTER` 宏, 是唯一不同的二进制。

**优化**: 一个二进制 + 软链接
```bash
HelloHttp/bin/HelloHttp  →  (实际文件)
HelloWs/bin/HelloWs      →  ln -s ../../HelloHttp/bin/HelloHttp
HelloHttps/bin/HelloHttps →  ln -s ../../HelloHttp/bin/HelloHttp
Logic/bin/Logic          →  ln -s ../../HelloHttp/bin/HelloHttp
Interface/bin/Interface  →  ln -s ../../HelloHttp/bin/HelloHttp
```
**省**: 4×43MB = **172 MB 磁盘, 更快的增量构建**

### 🟡 P2: 插件 .so 重复部署

| 插件 | 部署到 | 是否相同 | 浪费 |
|------|--------|---------|------|
| ModuleHello.so | HelloHttp/plugins/, HelloHttps/plugins/ | md5 相同 | 1 份冗余 |

**优化**: 共享 plugins 目录或软链接

### 🟡 P3: 4 种 IoBackend 全部编译进每个二进制

| 实现 | 代码行 | 运行时状态 | 建议 |
|------|--------|-----------|------|
| EvIoBackend | 313+72 | ✅ 主力 (ev) | 保留 |
| AsioUringIoBackend | 567+172 | ⚠️ OFF (编译选项关闭但代码还在) | 条件编译或删除 |
| NativeUringIoBackend | 424+110 | ❌ 待移除 (自己标注的) | 删除 |
| DpdkIoBackend | 179+101 | ❌ 无硬件 | 删除 |

**NativeUringIoBackend 自注释为"待移除"**, DpdkIoBackend 是"骨架占位", 但都通过 GLOB_RECURSE 编译进了每个二进制。

**优化**: 改 `file(GLOB_RECURSE)` 为显式源文件列表, IoBackend 按编译选项条件包含。

### 🟡 P4: 脚本重复

| 文件 | 副本数 | 重复内容 |
|------|--------|---------|
| script_func.sh | HelloHttp/HelloWs/HelloHttps (3份完全相同) | 启动/停止/状态检查函数 |
| node.sh | 7个, HelloWs==HelloHttps (2份相同) | 差异仅在工作目录和端口 |

**优化**: 一个 `lib/thunder_func.sh` + 各服务 `node.sh` 只写差异行(2-3行)

---

## 三、Config 层面冗余

### 按 io_backend 分裂的配置文件 (可合并)

```
HelloHttp/conf/
  Hello.json          ← ev 后端 (默认)
  Hello_ev.json       ← ev 后端 (冗余, 和 Hello.json 功能相同)
  Hello_asio.json     ← asio_uring 后端 (THUNDER_IO_ASIO_URING=OFF, 用不上)

HelloHttps/conf/
  HelloHttps.json     ← ev 后端
  HelloHttps_ev.json  ← ev 后端 (冗余)
  HelloHttps_asio.json← asio_uring 后端 (用不上)
```

6 个配置文件中有 4 个是为未启用的 io_backend 准备的。

### Center 多实例配置

```
Center/conf/Center.json   ← 节点1
Center/conf2/Center.json  ← 节点2
Center/conf3/Center.json  ← 节点3
```
3 个目录仅端口不同 (27000/27022/27032), 可用环境变量或命令行参数替代。

---

## 四、汇总: 可优化项与收益

| # | 项目 | 类型 | 磁盘 | 构建 | 运行时 | 复杂度降 |
|---|------|------|------|------|--------|---------|
| 1 | 5服务→软链接 | 二进制冗余 | -172MB | -80% copy时间 | 0 | ⭐⭐⭐ |
| 2 | 删除 NativeUring + Dpdk IoBackend | 死代码 | -5MB | -2个编译单元 | -内存 | ⭐⭐⭐ |
| 3 | AsioUring 条件编译 | 死代码 | -3MB | 跳过 567行编译 | -内存 | ⭐⭐ |
| 4 | script_func.sh → 共享1份 | 脚本冗余 | 0 | 0 | 0 | ⭐⭐ |
| 5 | node.sh 差异最小化 | 脚本冗余 | 0 | 0 | 0 | ⭐ |
| 6 | 合并 per-backend 配置 | 配置冗余 | 1KB | 0 | 0 | ⭐⭐ |
| 7 | Center 多实例用参数替代目录 | 目录冗余 | 0 | 0 | 0 | ⭐⭐ |
| 8 | GLOB_RECURSE → 显式源文件列表 | 编译安全 | 0 | 可能增量构建更快 | 0 | ⭐⭐⭐ |
| **合计** | | | **~180MB** | **-30%** | **-15%内存** | |

---

## 五、最优先行动 (可立即做, 零风险)

### 1. 二进制软链接 (1行改动)
```bash
# 把 5 个 43MB 拷贝换成软链接
cd deploy
rm HelloWs/bin/HelloWs HelloHttps/bin/HelloHttps Logic/bin/Logic Interface/bin/Interface
ln -s ../../HelloHttp/bin/HelloHttp HelloWs/bin/HelloWs
ln -s ../../HelloHttp/bin/HelloHttp HelloHttps/bin/HelloHttps
ln -s ../../HelloHttp/bin/HelloHttp Logic/bin/Logic
ln -s ../../HelloHttp/bin/HelloHttp Interface/bin/Interface
```

### 2. 删除待移除代码 (NatvieUringIoBackend 作者自己标注了"待移除")
```bash
git rm code/Net/src/labor/NativeUringIoBackend.cpp
git rm code/Net/src/labor/NativeUringIoBackend.hpp
```

### 3. 删 DpdkIoBackend (骨架, 无硬件)
```bash
git rm code/Net/src/labor/DpdkIoBackend.cpp
git rm code/Net/src/labor/DpdkIoBackend.hpp
```
