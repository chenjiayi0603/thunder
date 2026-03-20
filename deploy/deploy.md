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
cd deploy && ./install.sh first && ./restart_nodes.sh all

其他常用操作(一键部署中已包含)：
第一次安装依赖库(需要安装rar)
./install.sh first

编译所有代码（CMake，仓库根）
cmake -S . -B build && cmake --build build -j"$(nproc)" && cmake --install build

安装所有文件
./install.sh all

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


# 安装执行文件(在目录deploy)#
安装所有文件
./install.sh all

第一次安装所有文件（会解压第三方库）
./install.sh first

安装所有plugin
./install.sh plugin

安装所有系统动态库
./install.sh libs

安装所有bin
./install_bins.sh all

安装所有plugin
./install_plugins.sh all

安装所有系统动态库
./install_libs.sh all

修改服务器IP配置
./install.sh change

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

重启、启动、停止指定服务程序，如
./restart_nodes.sh Access
./start_nodes.sh Access
./stop_nodes.sh Access

#云环境更新节点代码
（1）编译
在联调环境编译插件代码、协议代码、网络库代码、通用库代码

如果协议代码、网络库代码、通用库代码有更新，则需要全部编译

如果只有插件代码更新，则只是编译插件代码

（2）测试
在联调环境测试正常

（3）拷贝
从联调环境目录 deploy拷贝目录（包含插件文件、协议文件、网络库文件、通用库文件）到云环境 目录 deploy
./bin  ./lib ./plugins 

查看被拷贝文件时间  
./install_nodes.sh list Group

（4）脚本一键安装运行
./install_nodes.sh Group

（5）手动安装、运行（使用脚本的则不用本步骤）
安装被拷贝文件到节点运行目录
./install_bins.sh Group && ./install_libs.sh Group && ./install_plugins.sh Group 
./install_bins.sh Msg && ./install_libs.sh Msg && ./install_plugins.sh Msg 

查看节点运行目录被安装文件时间 
ll ./Group/bin  ./Group/lib  ./Group/plugins   
ll ./Msg/bin  ./Msg/lib  ./Msg/plugins   

重启节点
./restart_nodes.sh Group


