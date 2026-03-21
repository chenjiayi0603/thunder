#! /bin/bash 

SERVER_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${SERVER_HOME}
SERVER_HOME=`pwd`

SERVER_BIN=${SERVER_HOME}/bin
SERVER_CONF=${SERVER_HOME}/conf
SERVER_LIB=${SERVER_HOME}/lib  
SERVER_3LIB=${SERVER_HOME}/../3lib
SERVER_LOG=${SERVER_HOME}/log

SERVER_CONF_WEB=${SERVER_HOME}/confweb

# libNet、MariaDB、protobuf 等：deploy/lib + code/3party（与 tests/start_interfaceserver.sh 一致）
_THUNDER_DEPLOY="$(cd "${SERVER_HOME}/.." && pwd)"
_CODE="$(cd "${_THUNDER_DEPLOY}/../code" && pwd)"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${SERVER_LIB}:${SERVER_3LIB}:${_THUNDER_DEPLOY}/lib:${_CODE}/3party/lib:${_CODE}/3party/lib/mariadb:${_CODE}/3party/protobuf/build"

LOG_FILE="${SERVER_HOME}/log/${SCRIPT_NAME}.log"

. ${SERVER_HOME}/scripts/script_func.sh

if [ $# == 0 ]; then 
	echo "do you want to stop im server process ${SERVER_HOME}? [yes|no]"
	read stop_im_server
	if [ "yes" != "$stop_im_server" ]
	then
	    echo "cancel"
	    exit 0
	fi
fi

server_bin_files=`ls ${SERVER_BIN}/`
for server_bin in $server_bin_files
do
    if [ -f "${SERVER_CONF}/${server_bin}.json" ]
    then
        target_server=`awk -F'"server_name"' '/server_name/{print $2}'  ${SERVER_CONF}/${server_bin}.json | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
        target_port=`awk -F'"inner_port"' '/inner_port/{print $2}'  ${SERVER_CONF}/${server_bin}.json | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
        target_server_tag=`echo "$target_server" | awk '{print substr($0,0,10)}'`
        echo ${target_server_tag}
        running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`
        if [ -n "$running_target_server_pid" ]
        then
            echo "kill $running_target_server_pid    $target_server"
            kill $running_target_server_pid
        fi
    fi
    if [ -f "${SERVER_CONF_WEB}/${server_bin}.json" ]
    then
        target_server=`awk -F'"server_name"' '/server_name/{print $2}'  ${SERVER_CONF_WEB}/${server_bin}.json | sed 's/ //g' | awk -F'[:",]' '{print $3}'`
        target_port=`awk -F'"inner_port"' '/inner_port/{print $2}'  ${SERVER_CONF_WEB}/${server_bin}.json | sed 's/ //g' | awk -F'[:",]' '{print $2}'`
        target_server_tag=`echo "$target_server" | awk '{print substr($0,0,10)}'`
        echo ${target_server_tag}
        running_target_server_pid=`netstat -apn 2>>/dev/null | grep -w $target_port | grep $target_server_tag | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}'`
        if [ -n "$running_target_server_pid" ]
        then
            echo "kill $running_target_server_pid    $target_server"
            kill $running_target_server_pid
        fi
    fi
done

