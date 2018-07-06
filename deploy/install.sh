#!/bin/bash 
#RUN_PATH=`dirname $0`
RUN_PATH=`pwd`
cd ${RUN_PATH}
lib3_path=/app/analysis/analysisServer/deploy/3lib

SRC_IP=18.68
#不填则自动识别目标物理地址为本物理机地址
DST_IP=

if [ $# -lt 1 ]; then 
    echo "USAGE: $0 param1" 
    echo "please input option[all,plugin,lib]"
	exit 1; 
fi

function check_config()
{
	echo "IP:"
	find ../ -maxdepth 5 ! -path "../code/*" ! -name install.sh -type f -name "*.json" -o -name "*.sql" -o -name "*.sh"|xargs grep -rin --color "192.168"
	echo "PORT:"
	find ../ -maxdepth 5 ! -path "../code/*" -type f -name "*.json"|xargs grep -rin --color "inner_port"
	find ../ -maxdepth 5 ! -path "../code/*" -type f -name "*.json"|xargs grep -rin --color "access_port"
}

function change_config()
{
	#FreeBSD/OpenBSD
	test -z "${DST_IP}" && DST_IP=`ifconfig|grep -E 'inet.[0-9]'|grep -v '192.'|awk '{print $2}'|awk 'NR==1{print}'`
	#linux
	test -z "${DST_IP}" && DST_IP=`ifconfig |grep "inet addr" |grep "192."|grep -o "addr:[0-9.]\{1,\}"|cut -d: -f2|awk 'NR==1{print}'`
	test -z "${DST_IP}" && echo "failed to get DST_IP" && exit 0
	
	#输入修改原地址
	#read -t 30 -p "请输入需要修改的原来的SRC_IP后面两个数字（如18.68）:" SRC_IP
	test -z "${SRC_IP}" && echo "SRC_IP empty" && exit 0
	SRC_IP="192.168.${SRC_IP}"
	test "${DST_IP}x" == "${SRC_IP}"x && echo "DST_IP:${DST_IP} and SRC_IP:${SRC_IP} are the same"
	
	echo "change config from SRC_IP:${SRC_IP} to DST_IP:${DST_IP}"
	
	find ./ -maxdepth 4 -type f -name "*.json"  |xargs sed -i "s/${SRC_IP}/${DST_IP}/g"
	test -d ../tools && find ../tools -maxdepth 3 -type f -name "*.sh"  |xargs sed -i "s/${SRC_IP}/${DST_IP}/g"
	test -d ../db/mysql && find ../db/mysql -maxdepth 3 -type f -name "db_*.sql"  |xargs sed -i "s/${SRC_IP}/${DST_IP}/g"
}

if [ "$1"x == "all"x ];then
	./install_bins.sh all && ./install_libs.sh all && ./install_plugins.sh all
elif [ "$1"x == "plugin"x ];then
	./install_plugins.sh all
elif [ "$1"x == "lib"x ];then
	./install_libs.sh all
elif [ "$1"x == "pre"x ];then
	find ./ -maxdepth 5 -type f -name "*.sh"  |xargs -i chmod +x {}
	find ../tools -maxdepth 3 -type f -name "*.sh"  |xargs -i chmod +x {}
	#3lib
	test ! -d ./3lib  && test -d ${lib3_path} && ln -s ${lib3_path} ${RUN_PATH}/3lib  && echo "ln 3lib for deploy"
	#config
	change_config
elif [ "$1"x == "core"x ];then
	find ./ -maxdepth 3 -type f -name "core*"  |xargs -i rm {}
elif [ "$1"x == "test"x ];then
	cd ../tools/siege/
	./siegeHello.sh one
elif [ "$1"x == "change"x ];then
	change_config
elif [ "$1"x == "check"x ];then
	check_config
elif [ "$1"x == "machine"x ];then
	#cpu
	echo "cpu lscpu" && lscpu
	echo "cat /proc/cpuinfo" && cat /proc/cpuinfo
	#mem
	echo "free -m"&& free -m
	echo "cat /proc/meminfo" && cat /proc/meminfo
	#disk
	echo "disk: lsblk" && lsblk
	#network
	echo "network: 查看网卡硬件信息 lspci | grep -i 'eth'" && lspci | grep -i 'eth'
	echo "查看系统的所有网络接口 ifconfig -a" && ifconfig -a
	echo "ip link show" && 	ip link show
	echo "sudo ethtool eth0" &&	sudo ethtool eth0
else
	echo "do nothings"
fi