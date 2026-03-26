#!/bin/bash

SERVER_HOME=`dirname $0`
cd ${SERVER_HOME}
SERVER_HOME=`pwd`
SERVER_CONF=${SERVER_HOME}/conf

BIN_NAME="HelloWs"
JSON_NAME="HelloWs.json"

if [ ! -f "${SERVER_CONF}/${JSON_NAME}" ]; then
    echo "missing ${SERVER_CONF}/${JSON_NAME}" >&2
    exit 1
fi

target_server=`awk -F'"server_name"' '/server_name/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
target_port=`awk -F'"inner_port"' '/inner_port/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
target_server_tag=`echo "$target_server" | awk '{print substr($0,0,10)}'`
running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`
if [ -n "$running_target_server_pid" ]; then
    echo "kill $running_target_server_pid    $target_server (${BIN_NAME})"
    kill $running_target_server_pid
else
    echo "no running process found for ${BIN_NAME} (inner_port ${target_port})"
fi

