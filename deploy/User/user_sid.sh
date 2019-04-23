#!/bin/bash
SERVER_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${SERVER_HOME}
SERVER_HOME=`pwd`

#用于设置最小统计id、获取统计id

MIN_USER_SID=1000

DST_PORT=7000
DST_IP=`ifconfig |grep "inet addr" |grep "192."|grep -o "addr:[0-9.]\{1,\}"|cut -d: -f2|awk 'NR==1{print}'`
chmod +x /app/analysis2/db/redis/bin/redis-cli 2>>/dev/null

function get_user_sid()
{
	user_sid=`/app/analysis2/db/redis/bin/redis-cli -h $DST_IP -p $DST_PORT get 4:2:user_sid`
	echo "user_sid:$user_sid"
}

function set_user_sid()
{
	user_sid=`/app/analysis2/db/redis/bin/redis-cli -h $DST_IP -p $DST_PORT get 4:2:user_sid`
	if [ $user_sid -lt $MIN_USER_SID ];then
		/app/analysis2/db/redis/bin/redis-cli -h $DST_IP -p $DST_PORT set 4:2:user_sid $MIN_USER_SID
		if [ $? -eq 0 ];then 
    		echo "succ to set 4:2:user_sid $MIN_USER_SID" 
    	else
    		echo "failed to set 4:2:user_sid $MIN_USER_SID" 
    	fi
    else
    	echo "$user_sid already not less then $MIN_USER_SID" 
	fi
}

if [ $# -lt 1 ]; then 
    echo "USAGE: $0 param1" 
    echo "please input option[get,set]"
	exit 1; 
fi

if [ "$1"x == "get"x ];then
	get_user_sid
elif [ "$1"x == "set"x ];then
	set_user_sid
	get_user_sid
fi
