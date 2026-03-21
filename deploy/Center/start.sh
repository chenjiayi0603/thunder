#!/bin/bash

SERVER_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${SERVER_HOME}
SERVER_HOME=`pwd`

SERVER_BIN=${SERVER_HOME}/bin
SERVER_CONF=${SERVER_HOME}/conf
SERVER_CONF2=${SERVER_HOME}/conf2
SERVER_CONF3=${SERVER_HOME}/conf3
SERVER_LIB=${SERVER_HOME}/lib  
SERVER_3LIB=${SERVER_HOME}/../3lib
SERVER_LOG=${SERVER_HOME}/log
# libNet、MariaDB、protobuf 等：deploy/lib + code/3party（与 tests/start_interfaceserver.sh 一致）
_THUNDER_DEPLOY="$(cd "${SERVER_HOME}/.." && pwd)"
_CODE="$(cd "${_THUNDER_DEPLOY}/../code" && pwd)"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${SERVER_LIB}:${SERVER_3LIB}:${_THUNDER_DEPLOY}/lib:${_CODE}/3party/lib:${_CODE}/3party/lib/mariadb:${_CODE}/3party/protobuf/build"

LOG_FILE="${SERVER_HOME}/log/${SCRIPT_NAME}.log"

. ${SERVER_HOME}/scripts/script_func.sh

function start_node()
{
	server_bin_files=`ls ${SERVER_BIN}/`
	for server_bin in $server_bin_files
	do
		local CONF="${1}/${server_bin}.json"
		#echo "$CONF"
	    if [ -f "$CONF" ]
	    then
	    	#echo "start ${SERVER_BIN}/$server_bin ${CONF}"
	        target_server=`awk -F'"server_name"' '/server_name/{print $2}'  ${CONF} | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
	        #echo "target_server:$target_server"
	        target_server_tag=`echo "$target_server" | awk '{print substr($0,0,20)}'`
	        #echo "target_server_tag:$target_server_tag"
	        target_port=`awk -F'"inner_port"' '/inner_port/{print $2}'  ${CONF} | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
	        #echo "target_port:$target_port"
	        running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`
	        #echo "running_target_server_pid:$running_target_server_pid"
	        if [ -z "$running_target_server_pid" ]
	        then
	            ${SERVER_BIN}/$server_bin ${CONF}
	            if [ $? -eq 0 ]
	            then
	                info_log "${SERVER_HOME}/bin/$server_bin start successfully."
	            else
	                error_log "failed to start $server_bin"
	            fi
	            ps -ef | awk -vpname=$target_server '{idx=index($8,pname); if (idx == 1)print}'
	        else
	            info_log "the server process for $server_bin was exist!"
	        fi
	    fi
	done
}

if [ "$1"x == "1"x ];then
	start_node "${SERVER_CONF}"
elif [ "$1"x == "2"x ];then
	start_node "${SERVER_CONF2}"
elif [ "$1"x == "3"x ];then
	start_node "${SERVER_CONF3}"
elif [ "$1"x == "all"x ];then
	start_node "${SERVER_CONF}"
	start_node "${SERVER_CONF2}"
	start_node "${SERVER_CONF3}"
else
	#默认启动单个中心节点
	start_node "${SERVER_CONF}"
	start_node "${SERVER_CONF2}"
	start_node "${SERVER_CONF3}"
fi



