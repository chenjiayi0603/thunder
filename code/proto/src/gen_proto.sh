ASYNC_SERVER_PATH=`dirname $0`
cd ${ASYNC_SERVER_PATH}
ASYNC_SERVER_PATH=`pwd`
ASYNC_SERVER_PATH_LIB=/app/analysis2/deploy/3lib
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${ASYNC_SERVER_PATH_LIB}

/app/analysis2/tools/bin/protoc  --version 
/app/analysis2/tools/bin/protoc -I=.. --cpp_out=. ../common.proto  ../behaviour.proto
