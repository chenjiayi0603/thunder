# 初始部署处理 #
拷贝代码目录code和部署目录deploy到目录/app/thunder
或者 建立软连接 类似
/home/chen/thunderworkon/code# mkdir /app
/home/chen/thunderworkon/code# ln -s /home/chen/thunderworkon /app/thunder

变更权限
find ./ -name "*.sh" |xargs -i chmod +x {}

解压第三方库
tar xvf 3lib.tar.gz 

第一次编译、部署和运行（在仓库根）：
cmake --build build --target Proto
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
cmake --install build
cd deploy && ./restart_nodes.sh all

其他常用操作：
编译所有代码（CMake，仓库根）
cmake -S . -B build && cmake --build build -j"$(nproc)" && cmake --install build

（旧版 `install.sh` / `install_*.sh` 已删除，请仅用 CMake 安装到 `deploy/`。）

启动所有节点
./start_nodes.sh all

# 程序目录 #
整个目录固定放置在 
/app/thunder

部署目录
/app/thunder/deploy

代码目录
/app/thunder/code

# 编译（已统一为 CMake，在仓库根执行） #
第一次编译并且部署（含协议、安装、重启节点）
见上文「第一次编译、部署和运行」

编译全部
cmake -S . -B build && cmake --build build -j"$(nproc)" && cmake --install build

单 target 示例
cmake --build build --target Net
cmake --build build --target Util
cmake --build build --target ModuleHello

多节点插件（Logic/Interface/Center）已由 CMake 构建，例如：
cmake --build build --target CmdGetToken ModuleInterface CmdElection

协议（CMake 生成 Proto）
cmake --build build --target Proto

说明：详见仓库根 `cmake/BUILD.md`、`INSTALL.md`。旧 makefile.center / makefile.other 与各节点 `src/Makefile` 仅作历史参考。


# 安装（在仓库根，CMake）#
```bash
cmake --build build -j"$(nproc)"
cmake --install build
```
默认安装前缀为 **`deploy/`**；详见仓库根 **`INSTALL.md`**、**`cmake/BUILD.md`**。

# 运行脚本及其节点配置(在目录deploy) #
服务节点插件路径
server_dir.conf 

运行启动、关闭服务节点配置
server_list.conf 

清理服务运行可执行文件和日志脚本
./clean.sh all  

重启、启动、停止所有服务程序文件脚本
./restart_nodes.sh all
./start_nodes.sh all
./stop_nodes.sh all

Interface 联调（Center → Logic → Interface，含 GenKey/VerifyKey 冒烟）：`./tests/start_interfaceserver.sh`

重启、启动、停止指定服务程序，如
./restart_nodes.sh Access
./start_nodes.sh Access
./stop_nodes.sh Access

# 云环境更新节点代码（概要）#
1. 在联调环境 **`cmake --build`** 所需 target，再 **`cmake --install build`**（或依赖 **`THUNDER_DEPLOY_AUTO`** 已拷贝到 `deploy/`）。
2. 测试通过后，将 **`deploy/`** 下对应 **`bin/`、`lib/`、各节点 `plugins/`** 同步到目标机 **`deploy/`**（按 **`server_dir.conf`** 布局）。
3. 目标机上 **`./restart_nodes.sh <组名>`** 或单节点脚本启停。


