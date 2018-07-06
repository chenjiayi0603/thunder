#!/bin/bash
#编译库文件
MAKE_PATH=`pwd`
cd ${MAKE_PATH}
RUN_PATH=${MAKE_PATH}/../deploy

proto_so=libim_proto.so

function make_librobot_proto()
{
	if [ -d ${MAKE_PATH}/proto ];then
		echo "making ${proto_so}"
	    #${proto_so}
	    cd ${MAKE_PATH}/proto
	    rm ${MAKE_PATH}/proto/src/*.o ${MAKE_PATH}/proto/src/*.pb.h ${MAKE_PATH}/proto/src/*.pb.cc
	    cd ${MAKE_PATH}/proto/src/
	    make clean
	    ./gen_proto.sh
	    make && cp -v ${MAKE_PATH}/proto/src/${proto_so} ${RUN_PATH}/lib 
	    echo "${RUN_PATH}/lib"
	    ls -l ${RUN_PATH}/lib 
    else
	    echo "make no proto" 
	fi
}

function clear_librobot_proto()
{
	if [ -d ${MAKE_PATH}/proto/src ];then
		cd ${MAKE_PATH}/proto/src
	    make clean 
	    test -f ${RUN_PATH}/lib/${proto_so} && unlink ${RUN_PATH}/lib/${proto_so} 
	else
		echo "clear no proto"
	fi
}

test ! -d ${MAKE_PATH}/l3oss/lib && ln -s ${RUN_PATH}/3lib ${MAKE_PATH}/l3oss/lib

if [ "$1"x == "clean"x ];then
    clear_librobot_proto
else
    clear_librobot_proto
    make_librobot_proto
fi


