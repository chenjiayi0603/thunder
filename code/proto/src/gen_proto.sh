ASYNC_SERVER_PATH=`dirname $0`
cd ${ASYNC_SERVER_PATH}
ASYNC_SERVER_PATH=`pwd`
ASYNC_SERVER_PATH_LIB=/app/thunder/deploy/3lib
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${ASYNC_SERVER_PATH_LIB}

/app/thunder/tools/bin/protoc  --version 
/app/thunder/tools/bin/protoc -I=.. --cpp_out=. ../common.proto  ../behaviour.proto
