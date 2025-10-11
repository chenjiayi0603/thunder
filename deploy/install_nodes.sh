#!/bin/bash 

: << !
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
!

#bin文件安装脚本
RUN_PATH=`pwd`
cd ${RUN_PATH}
config_file="server_dir.conf"
server_file="server_list.conf"
src_path="/bin/"

function Usage()
{
	echo -e "configure server_dir.conf  and  server_list.conf  before installation"
    echo -e "Usage: ./`basename $0` [OPTION]"
    echo -e "OPTION: [-h|--help/all/clean]"
    while read server others
    do 
    	echo "/${server}"
    done < ${server_file}
}

if [ $# -lt 1 ]; then 
    Usage
	exit 1; 
fi

function ListNodes()
{
	ls -l bin/ lib/ 
    while read nodetype others
    do 
        echo "${nodetype}:"
        #输出安装前文件
        find  ${RUN_PATH}${src_path} -type f -name "${nodetype}" | xargs -i ls -l --color=tty {}
        #输出安装后文件    
        find  ${RUN_PATH}${dest_path} -type f -name "${nodetype}" | xargs -i ls -l --color=tty {}
    done < ${config_file}
}

while  true :
do
    case "$1" in
        -h|--help)
            Usage
            break
            ;;
        nodes)
        	ListNodes
        	break
            ;;
        list)
        	if [ $# -eq 2 ]; then 
        		ls -l bin/$2 lib/ ./plugins/$2/ 
				exit 1; 
			fi
            break
            ;;
        *)
        	./install_bins.sh $1 && ./install_libs.sh $1 && ./install_plugins.sh $1 && ./restart_nodes.sh $1 && ls -l ./$1/bin  ./$1/lib  ./$1/plugins   
            break
            ;;
    esac
done


