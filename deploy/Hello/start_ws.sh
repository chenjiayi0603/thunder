#!/bin/bash
#
# 仅启动 WebSocket 接入节点（CODEC_WEBSOCKET_EX_JS，见 conf/HelloWs.json）。
# 与 start.sh 使用相同 Hello 可执行文件；若 bin/HelloWs 不存在则自动 ln -s Hello HelloWs。
#
# 用法: 在 deploy/Hello 目录下 ./start_ws.sh
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
BASE_BIN="Hello"
JSON_NAME="${WS_BIN}.json"

if [ ! -f "${SERVER_CONF}/${JSON_NAME}" ]; then
    error_log "missing ${SERVER_CONF}/${JSON_NAME}"
    exit 1
fi

if [ ! -x "${SERVER_BIN}/${BASE_BIN}" ]; then
    error_log "missing executable ${SERVER_BIN}/${BASE_BIN} (build/install Hello first)"
    exit 1
fi

if [ ! -x "${SERVER_BIN}/${WS_BIN}" ]; then
    ( cd "${SERVER_BIN}" && ln -sf "${BASE_BIN}" "${WS_BIN}" ) || {
        error_log "failed to symlink ${SERVER_BIN}/${WS_BIN} -> ${BASE_BIN}"
        exit 1
    }
    info_log "created ${SERVER_BIN}/${WS_BIN} -> ${BASE_BIN}"
fi

target_server=`awk -F'"server_name"' '/server_name/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
target_server_tag=`echo "$target_server" | awk '{print substr($0,0,20)}'`
target_port=`awk -F'"inner_port"' '/inner_port/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`

if [ -z "$running_target_server_pid" ]; then
    ${SERVER_BIN}/${WS_BIN} ${SERVER_CONF}/${JSON_NAME}
    if [ $? -eq 0 ]; then
        info_log "${SERVER_BIN}/${WS_BIN} start successfully."
    else
        error_log "failed to start ${WS_BIN}"
        exit 1
    fi
    ps -ef | awk -vpname=$target_server '{idx=index($8,pname); if (idx == 1)print}'
else
    info_log "the server process for ${WS_BIN} already exists (pid ${running_target_server_pid})"
fi
