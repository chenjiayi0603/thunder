#!/bin/bash 
#cmd so文件安装
#`dirname $0`
RUN_PATH=`pwd`
cd ${RUN_PATH}
config_file="server_dir.conf"
server_file="server_list.conf"

function Usage()
{
	echo -e "configure server_dir.conf  and  server_list.conf  before installation"
    echo -e "Usage: ./`basename $0` [OPTION]"
    echo -e "OPTION: [-h|--help/all/clean"
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

function PrintSo()
{
	while read nodetype dest_path src_path others
    do 
        echo "${nodetype}:"
        #输出安装前文件
        find  ${RUN_PATH}${src_path} -type f -name "*.so" | xargs -i ls -l --color=tty {}
        #输出安装后文件
        find  ${RUN_PATH}${dest_path} -type f -name "*.so" | xargs -i ls -l --color=tty {}
    done < ${config_file}
}

function InstallAll()
{
	cd ${RUN_PATH}
    while read nodetype dest_path src_path others
    do
    	test ! -d ${RUN_PATH}${dest_path} && mkdir -p ${RUN_PATH}${dest_path} &&　echo "mkdir -p ${RUN_PATH}${dest_path}"
        echo "find ${RUN_PATH}${src_path} -type f -name '*.so' | xargs -i install -vD {} ${RUN_PATH}${dest_path}"
        find ${RUN_PATH}${src_path} -type f -name "*.so" | xargs -i install -vD {} ${RUN_PATH}${dest_path}
    done < ${config_file}
    PrintSo
}

function InstallOne()
{
	#安装指定node的库文件
    cd ${RUN_PATH}
    echo "try to intall $1"
    while read nodetype dest_path src_path others
    do
    	test ! -d ${RUN_PATH}${dest_path} && mkdir -p ${RUN_PATH}${dest_path} &&　echo "mkdir -p ${RUN_PATH}${dest_path}"
        echo "checking ${RUN_PATH}${dest_path}"
        test "${nodetype}"x == "$1"x &&\
        echo "install ${RUN_PATH}${src_path} to ${RUN_PATH}${dest_path}" &&\
        find ${RUN_PATH}${src_path} -type f -name "*.so" | xargs -i install -vD {} ${RUN_PATH}${dest_path} &&\
        echo "${nodetype}:" &&\
        find  ${RUN_PATH}${src_path} -type f -name "*.so" | xargs -i ls -l --color=tty {} &&\
        find  ${RUN_PATH}${dest_path} -type f -name "*.so" | xargs -i ls -l --color=tty {} &&\
        echo "done" && exit 0
    done < ${config_file}
    echo "nothings to install"
}

function CleanAll()
{
	cd ${RUN_PATH}
    while read nodetype dest_path src_path others
    do
        echo "find ${RUN_PATH}${dest_path} -type f -name "*.so" | xargs -i unlink {}"
        find ${RUN_PATH}${dest_path} -type f -name "*.so" | xargs -i unlink {}
    done < ${config_file}
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
        clean)
            CleanAll
            break
            ;;
        *)
        	InstallOne "$1"
            break
            ;;
    esac
done
 
