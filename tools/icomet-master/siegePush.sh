#!/bin/bash
ROBOT_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${ROBOT_HOME}
ROBOT_HOME=`pwd`

#使用压测工具siege,如果没安装则 执行安装sudo yum install -y siege
SIEGE_BIN=`command -v /usr/local/bin/siege >/dev/null 2>&1  && echo "/usr/local/bin/siege" || echo "/usr/bin/siege"`
SIEGE_COMMAND="http://127.0.0.1:8000/push?cname=12&content=hi"
#/usr/bin/siege -c 1 -r 1 "\"http://127.0.0.1:8000/push?cname=12&content=hi\""

if [ $# -lt 1 ]; then 
	cmd="${SIEGE_BIN} -c 1 -r 1 '"${SIEGE_COMMAND}"'"
	echo $cmd
	eval $cmd
elif [ $# -eq 1 ]; then 
	if [[ $1 != *[!0-9]* ]]; then
		cmd="${SIEGE_BIN} -c 1 -r $1 '"${SIEGE_COMMAND}"'"
		echo $cmd
		eval $cmd
	else
		echo "do nothings"
	fi
elif [ $# -eq 2 ]; then 
	if [[ $1 != *[!0-9]* ]] && [[ $2 != *[!0-9]* ]]; then
		cmd="${SIEGE_BIN} -c $1 -r $2 '"${SIEGE_COMMAND}"'"
		echo $cmd
		eval $cmd
	else
		echo "do nothings"
	fi
else
	${SIEGE_BIN} -c 300 -r 300 "${SIEGE_COMMAND}"
fi

#netstat -an|grep CONNECTED|wc -l
