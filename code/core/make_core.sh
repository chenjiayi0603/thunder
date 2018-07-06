#!/bin/bash
#编译库文件
MAKE_PATH=`pwd`
cd ${MAKE_PATH}
RUN_PATH=${MAKE_PATH}/../../deploy

lib_so=libloss.so
core_so=libstarship.so

if [ $# -lt 1 ]; then 
    echo "USAGE: $0 param1" 
    echo "please input param1[all,loss,Starship,clean]"
    exit 1; 
fi

function make_loss()
{
	if [ -d ${MAKE_PATH}/loss ];then
		cd ${MAKE_PATH}/proto/src && chmod +x ./gen_proto.sh && ./gen_proto.sh && cd ${MAKE_PATH}
		test ! -d ${RUN_PATH}/lib && mkdir ${RUN_PATH}/lib
   		echo "making ${lib_so}"
	    #libloss.so
	    cd ${MAKE_PATH}/loss
	    make clean
	    make 
	    echo "${RUN_PATH}/lib"
	    ls -l  ${RUN_PATH}/lib
   	else
   		echo "make no lib"     
    fi
}

function make_plugins()
{
	if [ -f ${MAKE_PATH}/core_plugins.sh ];then
   		echo "making plugins"
   		cd ${MAKE_PATH}
	    ${MAKE_PATH}/core_plugins.sh all
   	else
   		echo "make no plugins"     
    fi
    
}

function make_bins_libstarship()
{
	if [ -d ${MAKE_PATH}/Starship/src ];then
		echo "making servers's bin   ${core_so}"
	    echo "cd ${MAKE_PATH}/Starship/src"
	    cd ${MAKE_PATH}/Starship/src
	    make clean 
	    make && find  ${MAKE_PATH}/Starship/src -type f -name "*Server" | xargs -i cp -v {} ${RUN_PATH}/bin && \
	    cp -v ${MAKE_PATH}/Starship/src/${core_so} ${RUN_PATH}/lib 
	    echo "${RUN_PATH}/bin"
	    ls -l ${RUN_PATH}/bin 
	    echo "${RUN_PATH}/lib"
	    ls -l ${RUN_PATH}/lib 
    else
		echo "make no Starship"    
	fi
}

function clear_loss()
{
	if [ -d ${MAKE_PATH}/Starship/src ];then
		cd ${MAKE_PATH}/loss
	    make clean
	    test -f ${RUN_PATH}/lib/libloss.so && unlink  ${RUN_PATH}/lib/libloss.so    
    else
	    echo "clear no lib"
	fi
}

function clear_Starship()
{
	if [ -d ${MAKE_PATH}/Starship/src ];then
		cd ${MAKE_PATH}/Starship/src
	    make clean 
	    test -f ${RUN_PATH}/lib/${core_so} && unlink  ${RUN_PATH}/lib/${core_so}
    else
    	echo "clear no Starship"
	fi
}

test ! -d ${MAKE_PATH}/l3oss/lib && ln -s ${RUN_PATH}/3lib ${MAKE_PATH}/l3oss/lib 

cd ${MAKE_PATH}

if [ $1 == "all" ];then
    make_loss 
    make_bins_libstarship
    make_plugins
elif [ $1 == "loss" ];then
    make_loss
elif [ $1 == "plugins" ];then
    make_plugins
elif [ $1 == "Starship" ];then
    make_bins_libstarship
elif [ $1 == "clean" ];then
    clear_loss
    clear_Starship
else
    echo "please input param1[all,loss,starship,clean]"
fi


