ASYNC_SERVER_PATH=`dirname $0`
cd ${ASYNC_SERVER_PATH}
ASYNC_SERVER_PATH=`pwd`
ASYNC_SERVER_PATH_LIB=/app/thunder/deploy/3lib
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${ASYNC_SERVER_PATH_LIB}:/app/thunder/tools/bin
echo "LD_LIBRARY_PATH"${LD_LIBRARY_PATH}

#和逻辑proto一起编译
chmod +x /app/thunder/tools/bin/protoc
/app/thunder/tools/bin/protoc  --version 
/app/thunder/tools/bin/protoc -I=.. --cpp_out=. ../user_basic.proto ../test_proto3.proto ../server.proto 
