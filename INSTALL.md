
# 编译依赖
解压 deploy/3lib.zip到deploy/3lib
如有编译符号找不到，则重新下载第三方库，重新编译并替换，第三方库所用版本参考code/3party/readme.txt

#   编译
编译所有的服务器执行文件
code/make.sh all

编译 proto、Net、Util
code/make_libs.sh all

编译 所有plugins
code/make_plugins.sh all

# 编译脚本配置说明
脚本时间通知配置

Net：makefile.access makefile.center makefile.other

节点子进程上报管理者、节点上报中心时间间隔配置
NODE_BEAT=10.0

子进程超时被重启时间配置
WORKER_OVERDUE=120.0

# 安装 
修改deploy下的配置文件，修改服务器ip
deploy/install.sh pre

安装所有的服务器执行文件到运行目录deploy
deploy/install.sh all

安装所有bin
deploy/install_bins.sh all

安装所有plugin
deploy/install_plugins.sh all

安装所有系统动态库
deploy/install_libs.sh all


# 启动
启动所有的服务器
deploy/start_nodes.sh all

关闭所有的服务器
deploy/stop_nodes.sh all

重启所有的服务器
deploy/restart_nodes.sh all

启动指定服务器
deploy/start_nodes.sh Interface

关闭指定服务器
deploy/stop_nodes.sh Interface

重启指定服务器
deploy/restart_nodes.sh Interface

配置启动、关闭服务节点
deploy/server_list.conf 启动、关闭服务节点配置

配置安装的服务可执行文件的路径
deploy/server_dir.conf 服务节点插件路径


清理可执行文件
deploy/clear.sh


重新加载插件so
deploy/restart_nodes.sh reload 重新加载所有正在运行的本机节点的插件so。目前只支持重新加载节点的服务配置，不支持重新加载so的逻辑代码。

在节点目录下
restart.sh reload 重新加载该节点正在运行的插件so。目前只支持重新加载节点的服务配置，不支持重新加载so的逻辑代码。

# 查看日志
查看服务器日志

中心服务器日志
deploy/Center/log/Center_robot.log
deploy/Center/log/Center_robot_W0.log

网关服务器日志
deploy/Interface/log/Interface_robot.log
deploy/Interface/log/Interface_robot_W0.log

网关服务器日志
deploy/Interface/log/Interface_robot.log
deploy/Interface/log/Interface_robot_W0.log

# 目录结构
deploy  运行目录
deploy/3lib  第三方库
deploy/Center  中心服务器
deploy/Center/log  中心服务器日志
deploy/Center/conf  中心服务器配置
deploy/Center/bin  中心服务器执行文件
deploy/Interface  网关服务器
deploy/Logic  逻辑服务器

code  代码目录
code/3party  第三方库
code/Interface  网关服务器
code/Logic  逻辑服务器

code/Core/Center  中心服务器
code/Core/Hello  测试服务器，网关服务器

code/Core/Net  网络库
code/Core/Util  工具库
code/Core/Proto  协议库

# 代码统计 #
脚本code.sh 

统计所有代码 code/code.sh all

统计框架代码 code/code.sh Net

统计指定目录代码 code/code.sh ./

# 测试
查看服务监听端口
lsof -Pni4 | grep LISTEN 

Interface\Logic\Center 启动后，Interface从Center注册发现Logic

消息流
client=>Interface =>Logic

测试指令：
测试生成token和key
curl "http://$(hostname -I | awk '{print $1}'):27008/Interface/gentoken"

测试验证token和key合法性
curl "http://$(hostname -I | awk '{print $1}'):27008/Interface/gentoken?token=7559256691418595329&key=7559256691418595330"
