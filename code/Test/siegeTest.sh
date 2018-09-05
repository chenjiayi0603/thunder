#!/bin/bash
ROBOT_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${ROBOT_HOME}
ROBOT_HOME=`pwd`
IP="192.168.18.78"
PORT=17137

#sudo yum install -y siege
SIEGE_BIN=`command -v /usr/local/bin/siege >/dev/null 2>&1  && echo "/usr/local/bin/siege" || echo "/usr/bin/siege"`
SIEGE_COMMAND="http://${IP}:${PORT}/hello/hello"

echo "length:" && echo "${FILE_DATA}" |wc -L

if [ $# -lt 1 ]; then 
	echo "wrong cmd" && exit 1
fi

test ! -f ${1} && echo "no such file ${1}" && exit 1

FILE_DATA=`cat ${1}`

if [ $# -eq 1 ]; then 
	${SIEGE_BIN} -c 1 -r 1 -b "${SIEGE_COMMAND} POST ${FILE_DATA}"
elif [ $# -eq 2 ]; then 
	echo "file ${1} $2"
	if [[ $2 != *[!0-9]* ]]; then
		echo "${SIEGE_BIN} -c 1 -r $2 ${SIEGE_COMMAND} POST ${FILE_DATA}"
		${SIEGE_BIN} -c 1 -r $2 -b "${SIEGE_COMMAND} POST ${FILE_DATA}"
	else
		echo "do nothings"
	fi
elif [ $# -eq 3 ]; then 
	echo "file ${1} $2 $3"
	if [[ $2 != *[!0-9]* ]] && [[ $3 != *[!0-9]* ]]; then
		echo "${SIEGE_BIN} -c $2 -r $3 ${SIEGE_COMMAND} POST ${FILE_DATA}"
		${SIEGE_BIN} -c $2 -r $3 -b "${SIEGE_COMMAND} POST ${FILE_DATA}"
	else
		echo "do nothings"
	fi
else
	${SIEGE_BIN} -c 300 -r 300 -b "${SIEGE_COMMAND} POST ${FILE_DATA}"
fi

connect=`netstat -an|grep CONNECTED|wc -l`
echo "CONNECTED:"$connect

