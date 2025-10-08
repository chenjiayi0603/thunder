# 数据恢复 #
 # 安装#
安装所有文件
deploy/install.sh all

安装所有bin
deploy/install_bins.sh all

安装所有plugin
deploy/install_plugins.sh all

安装所有系统动态库
deploy/install_libs.sh all

# 编译 #
编译 proto、Starship、loss
code/make_libs.sh all

编译 所有plugins
code/make_plugins.sh all

编译 所有库
code/make.sh all

# 编译脚本配置 #
脚本时间通知配置

Starship：makefile.access makefile.center makefile.other

中心节点：Makefile

节点子进程上报管理者、节点上报中心时间间隔配置
NODE_BEAT=10.0

子进程超时被重启时间配置
WORKER_OVERDUE=120.0

# 修改服务器配置 #
deploy/install.sh config

# 目录结构 #
server_dir.conf 服务节点插件路径

server_list.conf 启动、关闭服务节点配置

clear.sh清理服务运行文件脚本

install.sh安装服务程序文件脚本

restart_nodes.sh、start_nodes.sh、stop_nodes.sh重启、启动、停止服务程序文件脚本

# 代码统计 #
脚本code.sh 

统计所有代码 code.sh all

统计框架代码 code.sh frame

统计指定目录代码 code.sh ./

# https 相关接口增加 #
新增加使用库libcurl，及其库文件libcurl.so libcurl.so.4 libcurl.so.4.5.0。新增接口HttpsGet、HttpsPost

# 重新加载逻辑so文件 #
restart_nodes.sh reload 重新加载所有正在运行的本机节点的插件so。目前只支持重新加载节点的服务配置，不支持重新加载so的逻辑代码。

在节点目录下
restart.sh reload 重新加载该节点正在运行的插件so。目前只支持重新加载节点的服务配置，不支持重新加载so的逻辑代码。