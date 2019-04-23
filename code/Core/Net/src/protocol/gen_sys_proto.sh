<<<<<<< HEAD
ASYNC_SERVER_PATH=`dirname $0`
cd ${ASYNC_SERVER_PATH}
ASYNC_SERVER_PATH=`pwd`
ASYNC_SERVER_PATH_LIB=/app/analysis3/deploy/3lib
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${ASYNC_SERVER_PATH_LIB}
chmod +x /app/analysis3/deploy/3lib/protoc
/app/analysis3/deploy/3lib/protoc  --version 
/app/analysis3/deploy/3lib/protoc -I=. --cpp_out=.  ./http.proto  ./msg.proto ./oss_sys.proto 
=======
ASYNC_SERVER_PATH=`dirname $0`
cd ${ASYNC_SERVER_PATH}
ASYNC_SERVER_PATH=`pwd`
ASYNC_SERVER_PATH_LIB=/app/analysis3/deploy/3lib
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${ASYNC_SERVER_PATH_LIB}
chmod +x /app/analysis3/deploy/3lib/protoc
/app/analysis3/deploy/3lib/protoc  --version 
/app/analysis3/deploy/3lib/protoc -I=. --cpp_out=.  ./http.proto  ./msg.proto ./oss_sys.proto 
>>>>>>> branch 'master' of https://gitee.com/chenjiayi/thunder.git
