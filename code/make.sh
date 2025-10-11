#!/bin/bash
: << !
编译脚本说明：

第一次编译并且部署(需要编译哪些可执行文件节点，则配置 server.dep)
./make.sh first


编译全部(需要编译哪些可执行文件节点，则配置 server.dep)
./make.sh all

编译Net.so和可执行文件(需要编译哪些可执行文件节点，则配置 server.dep)
./make.sh Net

编译Util.so
./make.sh Util

编译插件逻辑so
./plugins.sh all

编译proto的so
./proto.sh 

!


MAKE_PATH=`pwd`
RUN_PATH=${MAKE_PATH}/../deploy
cd ${MAKE_PATH}

util_so=libUtil.so
net_so=libNet.so

function Usage()
{
	echo -e "plugins.conf is plugins conf dir,like  HelloServer 	/HelloServer/src  /plugins/HelloServer"
	echo -e "configure plugins.conf before compilation"
    echo -e "Usage: $0 [OPTION]..."
    echo -e "options: ./`basename $0` [-h|--help/Util/Net/Proto/all/plugin/clean/install]"
}

if [ $# -lt 1 ]; then 
    Usage
	exit 1; 
fi

function CleanUtil()
{
	if [ -d ${MAKE_PATH}/Net/src ];then
		cd ${MAKE_PATH}/Util
	    make clean
	    rm -rf ${MAKE_PATH}/Util/*.o ${MAKE_PATH}/Util/*.so
	    test -f ${RUN_PATH}/lib/libUtil.so && unlink  ${RUN_PATH}/lib/libUtil.so    
    else
	    echo "clear no lib"
	fi
}

function MakeUtil()
{
	if [ -d ${MAKE_PATH}/Util ];then
   		echo "making ${util_so}"
   		CleanUtil
	    cd ${MAKE_PATH}/Util
	    make 
	    echo "${RUN_PATH}/lib"
	    ls -l  ${RUN_PATH}/lib
	    return 0 
   	else
   		echo "make no lib"    
   		return 1 
    fi
}

function CleanNet()
{
	if [ -d ${MAKE_PATH}/Net/src ];then
		cd ${MAKE_PATH}/Net/src
	    make clean 
	    test -f ${RUN_PATH}/lib/${net_so} && unlink  ${RUN_PATH}/lib/${net_so}
    else
    	echo "clear no Net"
	fi
}

function MakeNet()
{
	if [ -d ${MAKE_PATH}/Net/src ];then
	    CleanNet
	    echo "making ${net_so}"
	    cd ${MAKE_PATH}/Net/src
	    make # && find  ${MAKE_PATH}/Net/src -type f -name "*Server" | xargs -i cp -v {} ${RUN_PATH}/bin && \
	    cp -v ${MAKE_PATH}/Net/src/${net_so} ${RUN_PATH}/lib 
	    echo "${RUN_PATH}/bin"
	    ls -l ${RUN_PATH}/bin 
	    echo "${RUN_PATH}/lib"
	    ls -l ${RUN_PATH}/lib 
	    return 0
    else
		echo "make no Net"    
		return 1 
	fi
}

function CleanProto()
{
	cd ${MAKE_PATH} && ./proto.sh clean
}

function MakeProto()
{
	cd ${MAKE_PATH} && ./proto.sh all
}

function CleanPlugins()
{
	cd ${MAKE_PATH} && ./plugins.sh clean
}

function MakePlugins()
{
	cd ${MAKE_PATH} && ./plugins.sh all
}

function MakeDir()
{
	#准备工作
	find ./ -maxdepth 5 -type f -name "*.sh"  |xargs -i chmod +x {}
	test ! -d ${RUN_PATH}/lib && mkdir ${RUN_PATH}/lib
	test ! -d ${MAKE_PATH}/3party/lib && test -d ${RUN_PATH}/3lib && ln -s ${RUN_PATH}/3lib ${MAKE_PATH}/3party/lib 
}

function CleanDeploy()
{
	#清除
	cd ${RUN_PATH} && ./clean.sh all
}

function InstallDeploy()
{
	#安装
	cd ${RUN_PATH} && ./install.sh all
}

function InstallFirst()
{
	#第一次安装
	cd ${RUN_PATH} && ./install.sh first
}

function RunDeploy()
{
	#运行
	cd ${RUN_PATH} && ./restart_nodes.sh all
}

MakeDir

while  true :
do
    case "$1" in
        -h|--help)
            Usage
            break
            ;;
        Util)
            MakeUtil
            break
            ;;
        Net)
            MakeNet
            break
            ;;
        clean)
        	CleanProto
            CleanUtil
    		CleanNet
    		CleanPlugins
            break
            ;;
        install)
            CleanDeploy
            InstallDeploy
            break
            ;;
        all)
        	#清理
        	CleanProto
            CleanUtil
    		CleanNet
    		CleanPlugins
			#编译
			cd ${MAKE_PATH} && MakeUtil && MakeNet && MakeProto && MakePlugins
            break
            ;;
        Proto)
            MakeProto
            break
            ;;
        plugin)
            MakePlugins
            break
            ;;
        first)
        	#清除
        	CleanDeploy
        	CleanProto
        	CleanUtil
    		CleanNet
        	CleanPlugins
        	
			#编译
			cd ${MAKE_PATH} && MakeUtil && MakeNet && MakeProto && MakePlugins

		    #安装
		    InstallFirst
		    
		    #运行
		    RunDeploy
            break
            ;;
        firstrun)
        	#安装
		    InstallFirst
		    #运行
		    RunDeploy
            break
			;;
        run)
            RunDeploy
            break
            ;;
        *)
            Usage
            break
            ;;
    esac
done
 
