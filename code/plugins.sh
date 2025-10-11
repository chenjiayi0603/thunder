#!/bin/bash 
#编译目录
MAKE_PATH=`pwd`
RUN_PATH=${MAKE_PATH}/../deploy
cd ${MAKE_PATH}
#plugins.conf 为逻辑节点插件列表目录,一行的格式为 HelloServer 	/HelloServer/src  /plugins/HelloServer

function Usage()
{
	echo -e "plugins.conf is plugins conf dir,like  HelloServer 	/HelloServer/src  /plugins/HelloServer"
	echo -e "configure plugins.conf before compilation"
    echo -e "Usage: $0 [OPTION]..."
    echo -e "options: ./`basename $0` [-h|--help/Center/all/clean]"
}

if [ $# -lt 1 ]; then 
    Usage
	exit 1; 
fi

function MakeDir()
{
	while read nodetype src_path dest_path  others
	do
	    test ! -d  ${RUN_PATH}${dest_path} && mkdir -p ${RUN_PATH}${dest_path}
	done < plugins.conf
}


function PrintSo()
{
	#输出编译后文件
    while read nodetype src_path dest_path others
    do 
        echo "${nodetype}:"
        find  ${RUN_PATH}${dest_path} -type f -name "*.so" | xargs -i ls -l --color=tty {}
    done < plugins.conf 
}

function CompileAll()
{
	cd ${MAKE_PATH}
    while read nodetype src_path dest_path others
    do
        test ! -d ${RUN_PATH}${dest_path} && mkdir -p ${RUN_PATH}${dest_path} &&　echo "mkdir -p ${RUN_PATH}${dest_path}"
        echo "cd ${MAKE_PATH}${src_path}" &&\
        cd ${MAKE_PATH}${src_path} &&\
        make clean && make && find ${MAKE_PATH}${src_path} -type f -name "*.so" | xargs -i cp -v {} ${RUN_PATH}${dest_path}
        cd ${MAKE_PATH}
    done < plugins.conf
    PrintSo
}

function CompileOne()
{
	cd ${MAKE_PATH}
    while read nodetype src_path dest_path others
    do
        test "${nodetype}"x == "$1"x &&\
        echo "cd ${MAKE_PATH}${src_path}" &&\
        cd ${MAKE_PATH}${src_path} &&\
        make clean && make && find ${MAKE_PATH}${src_path} -type f -name "*.so" | xargs -i cp -v {} ${RUN_PATH}${dest_path} &&\
        echo "${nodetype}:" &&\
        find  ${RUN_PATH}${dest_path} -type f -name "*.so" | xargs -i ls -l --color=tty {} &&\
        echo "done1" && exit 0
    done < plugins.conf
    echo "nothings to make"
    Usage
}

function CleanAll()
{
	cd ${MAKE_PATH}
    while read nodetype src_path dest_path others
    do
        test ! -d ${RUN_PATH}${dest_path} && mkdir -p ${RUN_PATH}${dest_path} &&　echo "mkdir -p ${RUN_PATH}${dest_path}"
        echo "cd ${MAKE_PATH}${src_path}" &&\
        cd ${MAKE_PATH}${src_path} &&\
        make clean 
        cd ${MAKE_PATH}
    done < plugins.conf
}

function CleanOne()
{
	cd ${MAKE_PATH}
    while read nodetype src_path dest_path others
    do
        test "${nodetype}"x == "$1"x &&\
        echo "cd ${MAKE_PATH}${src_path}" &&\
        cd ${MAKE_PATH}${src_path} &&\
        make clean &&\
        echo "${nodetype}:" &&\
        find  ${RUN_PATH}${dest_path} -type f -name "*.so" | xargs -i ls -l --color=tty {} &&\
        echo "done clean" 
    done < plugins.conf
    #echo "nothings to clear"
    #Usage
}

MakeDir

while  true :
do
    case "$1" in
        -h|--help)
            Usage
            break
            ;;
        all)
        	CleanAll
            CompileAll
            break
            ;;
        clean)
            CleanAll
            break
            ;;
        *)
        	CleanOne $1
            CompileOne $1
            break
            ;;
    esac
done
