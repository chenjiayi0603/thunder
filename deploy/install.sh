#!/bin/bash 
: << !
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

!

RUN_PATH=`pwd`
cd ${RUN_PATH}
lib3_path=/app/thunder/deploy/3lib
config_file="server_dir.conf"
server_file="server_list.conf"

SRC_IP=172.17.218.148
#不填则自动识别目标物理地址为本物理机地址
DST_IP=

function Usage()
{
	echo -e "configure server_dir.conf  and  server_list.conf  before installation"
    echo -e "Usage: ./`basename $0` [OPTION]"
    echo -e "OPTION: [-h|--help/all/clean/check/change"
    while read server others
    do 
    	echo "/${server}"
    done < ${server_file}
    echo -e "]"
}

if [ $# -lt 1 ]; then 
    Usage
	exit 1; 
fi

function CheckConfig()
{
	echo "IP:"
	find ../ -maxdepth 5 ! -path "../code/*" ! -name install.sh -type f -name "*.json" -o -name "*.sql" -o -name "*.sh"|xargs grep -rin --color "192.168"
	echo "PORT:"
	find ../ -maxdepth 5 ! -path "../code/*" -type f -name "*.json"|xargs grep -rin --color "inner_port"
	find ../ -maxdepth 5 ! -path "../code/*" -type f -name "*.json"|xargs grep -rin --color "access_port"
}

function CheckMachine()
{
	#cpu
	# 总核数 = 物理CPU个数 X 每颗物理CPU的核数 .   总逻辑CPU数 = 物理CPU个数 X 每颗物理CPU的核数 X 超线程数 .查看物理CPU个数
	echo "CPU physical number"
	cat /proc/cpuinfo| grep "physical id"| sort| uniq| wc -l
	# 查看每个物理CPU中core的个数(即核数)
	echo "CPU logic number"
	cat /proc/cpuinfo| grep "cpu cores"| uniq
	# 查看逻辑CPU的个数
	echo "CPU number"
	cat /proc/cpuinfo| grep "processor"| wc -l
	#mem
	echo "free -m"&& free -m
	df -h
	#network
	echo "sudo ethtool eth0" &&	sudo ethtool eth0
}

function ChangeConfig()
{
	find ./ -maxdepth 5 -type f -name "*.sh"  |xargs -i chmod +x {}
	#FreeBSD/OpenBSD
	test -z "${DST_IP}" && DST_IP=`ifconfig|grep -E 'inet.[0-9]'|grep -v '127.'|awk '{print $2}'|awk 'NR==1{print}'`
	#linux
	test -z "${DST_IP}" && DST_IP=`ifconfig |grep "inet addr" |grep "192."|grep -o "addr:[0-9.]\{1,\}"|cut -d: -f2|awk 'NR==1{print}'`
	test -z "${DST_IP}" && echo "failed to get DST_IP" && exit 0
	
	#输入修改原地址
	#read -t 30 -p "请输入需要修改的原来的SRC_IP后面两个数字（如18.68）:" SRC_IP
	test -z "${SRC_IP}" && echo "$SRC_IP empty" && exit 0
	
	echo "change config from SRC_IP:${SRC_IP} to DST_IP:${DST_IP}"
	find ./ -maxdepth 4 -type f -name "*.json"  |xargs sed -i "s/${SRC_IP}/${DST_IP}/g"
}

function PreProcess()
{
	# unzip ./3lib.zip
	# find ./ -maxdepth 5 -type f -name "*.sh"  |xargs -i chmod +x {}
	#3lib
	test ! -d ./3lib  && test -d ${lib3_path} && ln -s ${lib3_path} ${RUN_PATH}/3lib  && echo "ln 3lib for deploy"
	#config
	change_config
}

function DebugConfig()
{
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"log_level\":30000,/\"log_level\":0,/g"
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"log_level\":20000,/\"log_level\":0,/g"
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"process_num\":10,/\"process_num\":1,/g"
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"process_num\":6,/\"process_num\":1,/g"
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"process_num\":3,/\"process_num\":1,/g"
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"process_num\":2,/\"process_num\":1,/g"
}

function ReleaseConfig()
{
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"log_level\":0,/\"log_level\":20000,/g"
	find ./ -maxdepth 5 -type f -name "*.json"  |xargs sed -i "s/\"log_level\":10000,/\"log_level\":20000,/g"
	find ./ -maxdepth 5 ! -path "./Center/*" -type f -name "*.json"  |xargs sed -i "s/\"process_num\":1,/\"process_num\":3,/g"
}

function InstallAll()
{
	./install_bins.sh all && ./install_libs.sh all && ./install_plugins.sh all
}


while  true :
do
    case "$1" in
        -h|--help)
            Usage
            break
            ;;
        all)
            InstallAll  
            break
            ;;
        plugin)
            ./install_plugins.sh all
            break
            ;;
        libs)
            ./install_libs.sh all
            break
            ;;
        first)
        	PreProcess
        	InstallAll
            break
            ;;
        debug)
            PreProcess
            DebugConfig
            break
            ;;
        release)
            PreProcess
			ReleaseConfig
            break
            ;;
        check)
            CheckConfig
            CheckMachine
            break
            ;;
        change)
            ChangeConfig
            break
            ;;
        *)
        	echo "do nothings"
        	Usage
            break
            ;;
    esac
done
 