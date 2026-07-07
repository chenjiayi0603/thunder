# 部署说明（`deploy/`）

在**仓库根**用 CMake 构建与安装；默认安装前缀为 **`deploy/`**（详见仓库根 **`INSTALL.md`**、**`cmake/BUILD.md`**）。

## 首次构建与运行

**完整流程（含子模块与第三方）**见仓库根 **`INSTALL.md`** 中「一键」命令。

仅当 **`thirdparty_deploy` 已做过**、只需编主工程并安装时，可在仓库根：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j1 && cmake --install build

cd deploy && ./nodes.sh restart all
```

若需**先单独**拉协议再编其余，可在已有 `build/` 下：`cmake --build build --target Proto`，再全量 `cmake --build` / `cmake --install`。

## 常用命令

- 全量编译并安装（第三方已就绪时）：`cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j1 && cmake --install build`（首次请先看 **`INSTALL.md`** 一键流程）
- 在 `deploy/` 下启动所有节点：`./nodes.sh start all`

单 target 示例：

```bash
cmake --build build --target Net
cmake --build build --target Util
cmake --build build --target HelloPlugins
```

多节点插件：每个节点目录下可有**多个** `.so`；CMake 为每类节点提供**聚合 target**（依赖该节点全部插件库，见各 `code/<节点>/CMakeLists.txt` 末尾）：

```bash
cmake --build build --target LogicPlugins      # Logic 节点全部插件
cmake --build build --target InterfacePlugins  # Interface 节点全部插件
cmake --build build --target CenterPlugins     # Center 节点全部插件
cmake --build build --target HelloPlugins      # Hello 节点全部插件（示例）
```

新增某节点的 `.so` 时，在该节点 `CMakeLists.txt` 里 `add_library` 后，把新库名加入对应 `*Plugins` 的 `DEPENDS`。

全量构建时插件会随主工程一并生成；也可 **`cmake --build build && cmake --install build`** 安装到 `deploy/`。

协议（CMake 生成 Proto）：`cmake --build build --target Proto`

说明：业务代码已统一为 CMake；`code/3party` 内第三方上游自带的 Makefile 勿删。

## 安装（仓库根）

```bash
cmake --build build -j1
cmake --install build
```

## 运行脚本（在目录 `deploy/`）

统一部署脚本 **`nodes.sh`**（合并原 `start_nodes` / `stop_nodes` / `restart_nodes` / `clean` 及原 `server_list.conf`、`server_dir.conf` 配置；修改节点顺序或清理路径请编辑脚本内「配置」段）：

```text
./nodes.sh start all | <节点名>
./nodes.sh stop all | force | <节点名>
./nodes.sh restart all | reload | force | <节点名>
./nodes.sh restartforce all | <节点名>   # 强制重启（pkill 后再 start，stop 杀不掉时用）
./nodes.sh clean all | plugins | log | bin | core
```

Interface 联调（Center → Logic → Interface，含 GenKey/VerifyKey 冒烟）：
`python3 -m pytest tests/e2e -m "integration or smoke" --mode=local`

**Center 管理 CLI**（Python，`show` / `get` / `set`）：见 **`centercli/README_cn.md`**；仓库根示例：`python3 deploy/centercli/centercli.py --url http://<host>:<port>/admin show nodes`。

指定单节点示例：`./nodes.sh restart Logic`、`./nodes.sh start Interface`

## 云环境更新节点代码（概要）

1. 在联调环境 **`cmake --build`** 所需 target，再 **`cmake --install build`**（或依赖 **`THUNDER_DEPLOY_AUTO`** 已拷贝到 `deploy/`）。
2. 测试通过后，将 **`deploy/`** 下对应 **`bin/`、`lib/`、各节点 `plugins/`** 同步到目标机 **`deploy/`**（目录布局与 **`nodes.sh`** 内「配置」一致）。
3. 目标机上 **`./nodes.sh restart <节点名>`** 或 **`./nodes.sh start all`** 等启停。

