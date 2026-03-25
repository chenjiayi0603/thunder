#!/bin/bash
#
# 仅重启 WebSocket 接入节点（HelloWs）。
# 子命令与 restart.sh 对齐，但只针对 HelloWs：
#   ./restart_ws.sh          — stop_ws.sh + start_ws.sh
#   ./restart_ws.sh reload   — 向 HelloWs 主进程发 SIGUSR1（重载 so）
#   ./restart_ws.sh worker   — 发 SIGUSR2（重启 worker）
#

SERVER_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${SERVER_HOME}
SERVER_HOME=`pwd`
SERVER_BIN=${SERVER_HOME}/bin
SERVER_CONF=${SERVER_HOME}/conf

WS_BIN="HelloWs"
JSON_NAME="${WS_BIN}.json"

if [ $# == 1 ]; then
	if [ $1 == "reload" ]; then
		if [ -f "${SERVER_CONF}/${JSON_NAME}" ]; then
			target_server=`awk -F'"server_name"' '/server_name/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
			target_port=`awk -F'"inner_port"' '/inner_port/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
			target_server_tag=`echo "$target_server" | awk '{print substr($0,0,10)}'`
			running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`
			if [ -n "$running_target_server_pid" ]; then
				echo "reloading so files for ${WS_BIN}：$target_server pid:$running_target_server_pid"
				kill -SIGUSR1 $running_target_server_pid
			else
				echo "no running ${WS_BIN} for reload"
			fi
		fi
		exit 0
	elif [ $1 == "worker" ]; then
		if [ -f "${SERVER_CONF}/${JSON_NAME}" ]; then
			target_server=`awk -F'"server_name"' '/server_name/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
			target_port=`awk -F'"inner_port"' '/inner_port/{print $2}' ${SERVER_CONF}/${JSON_NAME} | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
			target_server_tag=`echo "$target_server" | awk '{print substr($0,0,10)}'`
			running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`
			if [ -n "$running_target_server_pid" ]; then
				echo "restart workers for ${WS_BIN}：$target_server pid:$running_target_server_pid"
				kill -SIGUSR2 $running_target_server_pid
			else
				echo "no running ${WS_BIN} for worker restart"
			fi
		fi
		exit 0
	fi
	echo "wrong command: $1 (use reload|worker or no args)"
	exit 1
fi

${SERVER_HOME}/stop_ws.sh -y
${SERVER_HOME}/start_ws.sh
