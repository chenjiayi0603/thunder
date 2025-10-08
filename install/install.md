 
##  七、安装代码
解压
cd /app/thunder/deploy
unzip 3lib.zip

编译
cd /app/thunder/code
./make.sh all

##  八、运行程序
cd /app/thunder/deploy

修改服务配置
sh install.sh pre
安装程序
./install.sh all
启动程序
./start_nodes.sh all
关闭程序
./stop_nodes.sh all
重启程序
./restart_nodes.sh all

