#!/bin/bash
ROBOT_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${ROBOT_HOME}
ROBOT_HOME=`pwd`
LOG_FILE="${ROBOT_HOME}/log/${SCRIPT_NAME}.log"

ssdb_host=192.168.18.78
ssdb_port=7000

es_ip=192.168.18.78
es_port=9200

bin=/app/thunder/db/redis/bin/redis-cli
INT_MAX=2147483647 

. ${ROBOT_HOME}/scripts/analysis_script_func.sh

#ssdb前缀区间查询的搜索词后缀（现在的规则） hlist ${word} ${word}\xff 10
search_postfix=\xff

test -z "$ssdb_host" && echo "invalid ssdb_host:$ssdb_host" && exit 1
test ! -f "$bin" && echo "not exist bin:$bin" && exit 1
test ! -d $datadir && mkdir $datadir

if [ $# -lt 1 ]; then 
	echo "wrong cmd.option:[$cmds]" && exit 1
fi
starttime=`date +'%Y-%m-%d %H:%M:%S'`
 
#测试
if [ "$1"x == "hmaketrace"x ]; then
	if [ $# -lt 2 ]; then 
		echo "hmake key(20180613)" && exit 0
	fi
	if [[ $2 == *[!0-9]* ]];then
		echo "$2 is no number" && exit 0
	fi
	if [[ $2 -gt 20180101 ]] && [[ $2 -lt 20300101 ]];then
		data=`cat testval.txt`
		cmd="$bin -h $ssdb_host -p $ssdb_port hset $trace?$2 6566748483057877193 '$data'"
		echo $cmd
    	eval $cmd
		$bin -h $ssdb_host -p $ssdb_port hget $trace?$2 6566748483057877193
	else
		echo "$2 need to be between (20180101,20300101)" && exit 0
	fi
elif [ "$1"x == "hgettest"x ]; then
	cmd="$bin -h $ssdb_host -p $ssdb_port hgetall ${key}1 |cut -d')' -f2|awk 'NR%2==0'"
	echo $cmd
	eval $cmd
	exit 0
elif [ "$1"x == "hscan"x ]; then
	if [ $# -lt 4 ]; then 
		echo "hscan name key_start key_end" && exit 1
	fi
	#hscan name key_start key_end limit
	cmd="$bin -h $ssdb_host -p $ssdb_port hscan $2 $3 $4 $INT_MAX|cut -d')' -f2|awk 'NR%2==0'>> hscan_$filename"
	echo $cmd
	eval $cmd
	exit 0
elif [ "$1"x == "hlist"x ]; then
	if [ $# -lt 3 ]; then 
		echo "hlist name_start name_end" && exit 1
	fi
	#hlist name_start name_end limit
	cmd="$bin -h $ssdb_host -p $ssdb_port hlist $2 $3 $INT_MAX"
	echo $cmd
	eval $cmd
	exit 0
elif [ "$1"x == "hscanname"x ]; then
	if [ $# -lt 2 ]; then 
		echo "hscan name" && exit 1
	fi
	#hscan name key_start key_end limit
	cmd="$bin -h $ssdb_host -p $ssdb_port hscan $2 "" "" $INT_MAX |cut -d')' -f2|awk 'NR%2==0'>> hscanname_$filename"
	echo $cmd
	eval $cmd
	echo "$filename"
	exit 0
else
	echo "wrong cmd.option:[$cmds]" && exit 1
	exit 0
fi


endtime=`date +'%Y-%m-%d %H:%M:%S'`
start_seconds=$(date --date="$starttime" +%s);
end_seconds=$(date --date="$endtime" +%s);
echo "run time:"$((end_seconds-start_seconds))"s"
