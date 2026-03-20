# 第三方库（`code/3party`）

子模块列表见仓库根 **`.gitmodules`**。拉取（**不要**写字面量 `...`，以下为完整路径；或一次性拉全仓库子模块）：

```bash
git submodule update --init --recursive code/3party/c-ares code/3party/curl code/3party/cryptopp \
  code/3party/hiredis-vip code/3party/libev code/3party/log4cplus code/3party/mariadb-connector-c \
  code/3party/mongo-c-driver code/3party/protobuf
# 或：git submodule update --init --recursive
```

**注意**：`log4cplus` 自身还依赖嵌套子模块 **`threadpool`**（提供 `ThreadPool.h`），必须用 **`--recursive`**，或事后执行：

```bash
cd code/3party/log4cplus && git submodule update --init threadpool catch
```

---

## CMake 统一编译（推荐）

根目录 **`CMakeLists.txt`** 已通过 **`add_subdirectory(code/3party)`** 引入本文件；在仓库根与主工程共用 **`build/`** 即可，无需单独进本目录配置。

按依赖顺序编译各子模块，安装到 **`<build>/code/3party/stage/`**（从根配置时）或 **`build-3party/stage/`**（仅在本目录 `cmake -S . -B build-3party` 时），再一键部署到：

| 目标 | 说明 |
|------|------|
| **`code/3party/lib`** | 动态库（及 `protoc` 可执行文件副本） |
| **`deploy/3lib`** | 与上表相同文件，**覆盖**同名已有第三方库 |
| **`code/3party/include`** | 合并安装头文件（与已有子目录合并覆盖） |

**Protobuf** 仍编译在源码树 **`code/3party/protobuf/build`**（主工程 `ThunderCommon.cmake` 需在此目录查找 `libprotobuf`、`utf8_validity` 与 **absl** 静态库）；部署步骤会额外把 `libprotobuf*.so`、`libprotoc*.so`、`protoc`、`utf8_range` 下相关 `.so` 复制到 `lib` / `3lib`。

**方式 A（推荐，与主工程同一 build 目录）**

```bash
cd /path/to/thunder   # 仓库根
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
# 可选：-DOPENSSL_ROOT_DIR=/path/to/openssl
cmake --build build --parallel --target thirdparty_deploy
```

**方式 B（仅在 `code/3party` 下单独配置）**

```bash
cd code/3party
cmake -S . -B build-3party -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-3party --parallel
cmake --build build-3party --target thirdparty_deploy
```

- **目标 `thunder_3party_all`**：仅编译全部第三方，不复制。  
- **目标 `thirdparty_deploy`**：依赖上述编译完成，再执行 **`deploy_thirdparty.sh`** 复制到 `lib` / `include` / `deploy/3lib`。

**未包含**：**jemalloc** 不在 `.gitmodules` 中，请使用系统包（如 `libjemalloc-dev`）或自行编译后放入 `deploy/3lib`。  
**首次编译 Protobuf**：若需联网拉取 **Abseil** 等依赖，请保证网络可用；若 **`protobuf/build`** 曾有旧缓存导致配置失败，可先删除该目录再配置。

更细的手动安装步骤（历史版本说明）见同目录 **`readme1.md`**。

---

## 主工程链接哪些库？

与 **`cmake/ThunderCommon.cmake`** 中 `thunder_link_thirdparty_shared` 一致，需能在 **`code/3party/lib`**（及 **`protobuf/build`**）找到对应 `.so` / `.a`。

---

## 与 `code/make.sh` 的关系

`code/make.sh` 在缺少 `code/3party/lib` 时会把其 **软链接到 `deploy/3lib`**。完成 **`thirdparty_deploy`** 后，两处的 `.so` 应一致。

---

## 历史：版本与手动安装（`readme1.md`）

详细版本号与逐步 wget/make 流程见 **`readme1.md`**。当前子模块中的 **protobuf** 上游版本可能已高于文档中的 3.6.1，以 **`code/3party/protobuf`** 实际为准。

**说明**：主工程已移除 **LevelDB** 依赖；若 `deploy/3lib` 中仍有 `libleveldb.so` 可不再使用。

### 若 `thirdparty_deploy` 在「全部 ep_* 编完」后仍报 Error 2

历史上曾用 `chmod +x deploy_thirdparty.sh`，在 **WSL / Windows 挂载盘** 等环境下 `chmod` 会失败并导致该 target 直接失败；现已改为 **`bash deploy_thirdparty.sh`**。若仍失败，可在仓库根手动执行看具体报错：

```bash
bash code/3party/deploy_thirdparty.sh "$(pwd)" \
  "build/code/3party/stage" "code/3party" "deploy/3lib" "code/3party/protobuf/build"
```
（路径按你的 `build` 目录调整。）

### 若第三方某一步失败（尤其改过 `CMakeLists.txt` 后）

可删掉对应 **ExternalProject** 缓存再编，例如 MariaDB：

```bash
rm -rf build/code/3party/ep/mariadb
cmake -S . -B build
cmake --build build --target ep_mariadb --parallel
```
