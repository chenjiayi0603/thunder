#! /bin/bash
#
# 仅停止 WebSocket 接入节点（与 conf/HelloWs.json / inner_port 对应）。
#
# 用法: 在 deploy/Hello 目录下 ./stop_ws.sh
#       无参数时与 stop.sh 相同，会提示确认；传入任意参数则跳过确认（例如 ./stop_ws.sh -y）。
#

SERVER_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${SERVER_HOME}
SERVER_HOME=`pwd`

SERVER_BIN=${SERVER_HOME}/bin
SERVER_CONF=${SERVER_HOME}/conf
SERVER_LIB=${SERVER_HOME}/lib
SERVER_3LIB=${SERVER_HOME}/../3lib
SERVER_LOG=${SERVER_HOME}/log
_THUNDER_DEPLOY="$(cd "${SERVER_HOME}/.." && pwd)"
_CODE="$(cd "${_THUNDER_DEPLOY}/../code" && pwd)"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${SERVER_LIB}:${SERVER_3LIB}:${_THUNDER_DEPLOY}/lib:${_CODE}/3party/lib:${_CODE}/3party/lib/mariadb:${_CODE}/3party/protobuf/build"

LOG_FILE="${SERVER_HOME}/log/${SCRIPT_NAME}.log"

. ${SERVER_HOME}/scripts/script_func.sh

WS_BIN="HelloWs"
JSON_NAME="${WS_BIN}.json"

if [ $# == 0 ]; then
	echo "do you want to stop WebSocket Hello (${SERVER_HOME} ${JSON_NAME})? [yes|no]"
	read stop_im_server
	if [ "yes" != "$stop_im_server" ]; then
	    echo "cancel"
	    exit 0
	fi
fi

if [ ! -f "${SERVER_CONF}/${JSON_NAME}" ]; then
	echo "missing ${SERVER_CONF}/${JSON_NAME}" >&2
	exit 1
fi

target_server=`awk -F'"server_name"' '/server_name/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
target_port=`awk -F'"inner_port"' '/inner_port/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
target_server_tag=`echo "$target_server" | awk '{print substr($0,0,10)}'`
echo "${target_server_tag}"
running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`
if [ -n "$running_target_server_pid" ]; then
    echo "kill $running_target_server_pid    $target_server (${WS_BIN})"
    kill $running_target_server_pid
else
    echo "no running process found for ${WS_BIN} (inner_port ${target_port})"
fi
