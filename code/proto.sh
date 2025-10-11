#!/bin/bash
#编译库文件
MAKE_PATH=`pwd`
RUN_PATH=${MAKE_PATH}/../deploy
cd ${MAKE_PATH}

proto_so=libProto.so

function MakeDir()
{
	test ! -d ${MAKE_PATH}/3party/lib && ln -s ${RUN_PATH}/3lib ${MAKE_PATH}/3party/lib
}

function ClearProto()
{
	if [ -d ${MAKE_PATH}/Proto/src ];then
		echo "clearing ${proto_so}"
		cd ${MAKE_PATH}/Proto/src
	    make clean 
	    rm -rf ${MAKE_PATH}/Proto/src/*.o ${MAKE_PATH}/Proto/src/*.pb.h ${MAKE_PATH}/Proto/src/*.pb.cc
	    test -f ${RUN_PATH}/lib/${proto_so} && unlink ${RUN_PATH}/lib/${proto_so} 
	else
		echo "clear no proto"
	fi
}

function MakeProto()
{
	if [ -d ${MAKE_PATH}/Proto ];then
		ClearProto
		echo "making ${proto_so}"
	    cd ${MAKE_PATH}/Proto/src/
	    ./gen_proto.sh
	    make && cp -v ${MAKE_PATH}/Proto/src/${proto_so} ${RUN_PATH}/lib 
	    echo "${RUN_PATH}/lib"
	    ls -l ${RUN_PATH}/lib 
    else
	    echo "make no proto" 
	fi
}

MakeDir

while  true :
do
    case "$1" in
        clean)
            ClearProto
            break
            ;;
        *)
            MakeProto
            break
            ;;
    esac
done


