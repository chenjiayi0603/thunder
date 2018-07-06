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

bin=/app/analysis2/db/redis/bin/redis-cli
INT_MAX=2147483647 

. ${ROBOT_HOME}/scripts/analysis_script_func.sh

trace=1:100:TRACE
user=1:100:USER
device=1:100:DEVICE

#用户信息-1:1:user?${user_sid}-ssdb hash
userinfo=1:1:user
#用户全局统计id-  4:2:user_sid-ssdb string
user_sid=4:2:user_sid
#用户统计id映射-  4:3:useridmap?${user_id}-ssdb string
useridmap=4:3:useridmap

#设备信息-1:4:device?${device_id}-ssdb hash
deviceinfo=1:4:device

#ssdb前缀区间查询的搜索词后缀（现在的规则） hlist ${word} ${word}\xff 10
search_postfix=\xff

datadir=data/
#大约多少行一个文件
filesize=10000
#测试
key=nametest
filename=`date +'%Y-%m-%d_%H:%M:%S'`.dat

test -z "$ssdb_host" && echo "invalid ssdb_host:$ssdb_host" && exit 1
test ! -f "$bin" && echo "not exist bin:$bin" && exit 1
test ! -d $datadir && mkdir $datadir

cmds="trace keys/user keys/device keys/trace contents/trace contents/trace contents/trace 20111101 20111102/user 20111101 20111102/device 20111101 20111102 all/trace 20111101 20111102 all/user 20111101 20111102 all/device 20111101 20111102 all"

if [ $# -lt 1 ]; then 
	echo "wrong cmd.option:[$cmds]" && exit 1
fi
starttime=`date +'%Y-%m-%d %H:%M:%S'`

#消息
function event_keys()
{
	filename=$datadir$1
	test -f "$filename" && rm "$filename" && echo "rm $filename"
	count=`$bin -h $ssdb_host -p $ssdb_port hlist $2 ${2}${search_postfix} $INT_MAX |wc -l`
	echo "count:$count"
	if [ $count -gt 0 ];then
		cmd="$bin -h $ssdb_host -p $ssdb_port hlist $2 ${2}${search_postfix} $INT_MAX |cut -d')' -f2 |awk '{if(length!=0)print}'>> $filename"
		echo $cmd
		eval $cmd
		fileline=`wc -l $filename|awk '{print $1}'`
		echo "fileline:$fileline"
		if [ $fileline -eq 0 ];then
			test -f "$filename" && rm "$filename"
		else
			echo "$filename:" && head $filename
		fi
	fi
}

function event_contents()
{
	if [[ $2 != *[!0-9]* ]] && [[ $3 != *[!0-9]* ]]; then
		if [[ $2 -gt 20180101 ]] && [[ $2 -lt 20300101 ]] && [[ $3 -gt 20180101 ]] && [[ $3 -lt 20300101 ]] && [[ $2 -le $3 ]];then
			#事件消息1:100:TRACE?20111101-ssdb hash
			#hscan name key_start key_end limit
			fileline=0
			index=0
			#删除之前的同类的文件
			cmd="rm ${datadir}*.dat >/dev/null 2>&1"
			echo $cmd
			eval $cmd
			filename=$datadir$1_$2_$3_$index.dat
			for((i=${2};i<=${3};i++));  
			do 
				count=`$bin -h $ssdb_host -p $ssdb_port hsize $4?$i`
				echo "event_contents count:$count for $4?$i"
				if [ $count -gt 0 ];then
					cmd="$bin -h $ssdb_host -p $ssdb_port hscan $4?$i \"\" \"\" $INT_MAX |cut -d')' -f2|awk '{if(length!=0&&length!=1)print}'|awk 'NR%3==2||NR%3==0'>> $filename"
					echo $cmd
					eval $cmd
					fileline=`wc -l $filename| awk '{print $1}'`
					echo "fileline:$fileline"
					if [ $fileline -gt $filesize ];then
						let ++index
						filename=$datadir$1_$2_$3_$index.dat
					fi
				fi
			done
			test -f $filename && echo "$filename:" && head $filename
		else
			echo "$2 and $3 has to be between 20180101 and 20300101.and $2 <= $3" && exit 1
		fi
	else
		echo "$2 or $3 is no number" && exit 1
	fi
}

function event_all()
{
	if [[ $2 != *[!0-9]* ]] && [[ $3 != *[!0-9]* ]]; then
		if [[ $2 -gt 20180101 ]] && [[ $2 -lt 20300101 ]] && [[ $3 -gt 20180101 ]] && [[ $3 -lt 20300101 ]] && [[ $2 -le $3 ]];then
			filename=$datadir$1_$2_$3
			test -f "$filename" && rm "$filename" && echo "rm $filename"
			#事件消息1:100:TRACE?20111101-ssdb hash
			#hscan name key_start key_end limit
			for((i=${2};i<=${3};i++));  
			do 
				count=`$bin -h $ssdb_host -p $ssdb_port hsize $4?$i`
				echo "event_contents count:$count for $4?$i"
				if [ $count -gt 0 ];then
					cmd="$bin -h $ssdb_host -p $ssdb_port hscan $4?$i \"\" \"\" $INT_MAX |cut -d')' -f2|awk '{if(length!=0&&length!=1)print}'>> $filename"
					echo $cmd
					eval $cmd
				fi
			done
			test -f $filename && echo "$filename:" && head $filename
		else
			echo "$2 and $3 has to be between (20180101,20300101).and $2 <= $3" && exit 1
		fi
	else
		echo "$2 or $3 is no number" && exit 1
	fi
}

function upload_es()
{
	filelist=`ls ${ROBOT_HOME}/${datadir}`
	for filename in $filelist ; do
	    if [ -f ${ROBOT_HOME}/${datadir}/${filename} ] && [ "${filename##*.}"x = "dat"x ]; then 
	        #没被占用的文件才能提交分析
	        pid=`lsof ${ROBOT_HOME}/${datadir}/${filename} | awk 'END{print $2}'`
	        if [ -z "$pid" ];then
	            filesize=`ls -ld ${ROBOT_HOME}/${datadir}/${filename} | awk '{print int($5)}'`
	            #echo "filename:${filename} last_upload:${last_upload}"
	            if [ $filesize != 0 ];then
	            	cmd="curl  -H 'Content-Type: application/json;charset=UTF-8' -XPOST '${es_ip}:${es_port}/_bulk?pretty' --data-binary @${ROBOT_HOME}/${datadir}/${filename}"
	            	echo $cmd
	            	eval $cmd >/dev/null 2>>${datadir}/upload_error.txt
	            	if [ $? -eq 0 ];then 
	            		echo "succ to upload ${ROBOT_HOME}/${datadir}/${filename}" 
	            		rm ${ROBOT_HOME}/${datadir}/${filename}
	            	else
	            		echo "failed to upload ${ROBOT_HOME}/${datadir}/${filename}" 
	            	fi
	            	echo ${filename} >> ${datadir}/uploaded_files.txt
		        else
		        	warn_log "del empty file:${ROBOT_HOME}/${datadir}/${filename}"
		        	rm ${ROBOT_HOME}/${datadir}/${filename}
	            fi
	        fi
		else
	        debug_log "File ${ROBOT_HOME}/${datadir}/${filename} is not a dat file."
	    fi 
	done
	
	cmd="curl '${es_ip}:${es_port}/_cat/indices?v'"
	echo $cmd
	eval $cmd
	
	#curl '192.168.18.78:9200/db_trace/_search?pretty'
	#curl '192.168.18.78:9200/db_trace/tb_trace/_search?pretty'
}

#消息检索与同步es
if [ "$1"x == "uploades"x ]; then
	upload_es
elif [ "$1"x == "trace"x ] || [ "$1"x == "user"x ] || [ "$1"x == "device"x ]; then
	#消息
	#事件消息1:100:TRACE?20111101-ssdb hash
	#用户消息1:100:USER?20111101-ssdb hash
	#设备消息1:100:DEVICE?20111101-ssdb hash
	keys_file=$1_keys
	keys_pre=
	test "$1"x == "trace"x && keys_pre=$trace
	test "$1"x == "user"x && keys_pre=$user
	test "$1"x == "device"x && keys_pre=$device
	test -z $keys_pre && echo "keys_pre empty" && exit 1
	if [ $# -eq 2 ]; then
		if [ "$2"x == "keys"x ]; then
			event_keys $keys_file $keys_pre
		elif [ "$2"x == "contents"x ]; then
			event_keys $keys_file $keys_pre
			if [ -f $datadir/$keys_file ];then
				line_h=`head -1 $datadir/$keys_file|cut -d'?' -f2`
				line_t=`tail -1 $datadir/$keys_file|cut -d'?' -f2`
				echo "line_h $line_h"
				echo "line_t $line_t"
				event_contents $1 $line_h $line_t $keys_pre
				rm $datadir/$keys_file
			fi
		else
			echo "$1 keys/$1 contents/$1 20111101 20111102/$1 20111101 20111102 all"
		fi
	elif [ $# -eq 3 ]; then
		event_contents $1 $2 $3 $keys_pre
	elif [ $# -eq 4 ]; then
		if [ "$4"x == "all"x ]; then 
			event_all $1 $2 $3 $keys_pre
		else
			echo "$1 keys/$1 contents/$1 20111101 20111102/$1 20111101 20111102 all"
		fi
	else
		echo "$1 keys/$1 contents/$1 20111101 20111102/$1 20111101 20111102 all"
	fi
#属性信息检索
elif [ "$1"x == "userids"x ]; then
	echo "$user_sid" 
	cmd="$bin -h $ssdb_host -p $ssdb_port get $user_sid"
	echo $cmd
	eval $cmd
elif [ "$1"x == "usercount"x ]; then
	count=`$bin -h $ssdb_host -p $ssdb_port keys ${useridmap} ${useridmap}${search_postfix} $INT_MAX|wc -l`
	echo "useridmap count:$count"
	count=`$bin -h $ssdb_host -p $ssdb_port hlist ${userinfo} ${userinfo}${search_postfix} $INT_MAX|wc -l`
	echo "userinfo count:$count"
elif [ "$1"x == "userclear"x ]; then
	if [ "$2"x == "yes"x ]; then
		#clear useridmap 4:3:useridmap?${user_id}
		echo "$bin -h $ssdb_host -p $ssdb_port keys ${useridmap} ${useridmap}${search_postfix} $INT_MAX"
		$bin -h $ssdb_host -p $ssdb_port keys ${useridmap} ${useridmap}${search_postfix} $INT_MAX |cut -d')' -f2|awk '{if(length!=0)print}'|xargs -i $bin -h $ssdb_host -p $ssdb_port del {} > /dev/null
		count=`$bin -h $ssdb_host -p $ssdb_port keys ${useridmap} ${useridmap}${search_postfix} $INT_MAX|wc -l`
		echo "useridmap count:$count"
		
		#clear user  1:1:user?${user_sid}
		$bin -h $ssdb_host -p $ssdb_port hlist ${userinfo} ${userinfo}${search_postfix} $INT_MAX |cut -d')' -f2|awk '{if(length!=0)print}'|xargs -i $bin -h $ssdb_host -p $ssdb_port hclear {} > /dev/null
		count=`$bin -h $ssdb_host -p $ssdb_port hlist ${userinfo} ${userinfo}${search_postfix} $INT_MAX|wc -l`
		echo "userinfo count:$count"
	fi
elif [ "$1"x == "useridmap"x ]; then
	if [ $# -lt 2 ]; then
		echo "useridmap userid" && exit 0
	fi
	cmd="$bin -h $ssdb_host -p $ssdb_port get ${useridmap}?${2}"
	echo $cmd
	eval $cmd
elif [ "$1"x == "userinfos"x ]; then
	#用户信息-1:1:user?${user_sid}-ssdb hash
	$bin -h $ssdb_host -p $ssdb_port hlist "${userinfo}" "${userinfo}${search_postfix}" $INT_MAX 
elif [ "$1"x == "userinfo"x ]; then
	if [ $# -lt 2 ]; then
		echo "userinfo userid" && exit 0
	fi
	$bin -h $ssdb_host -p $ssdb_port hgetall ${userinfo}"?"${2}
elif [ "$1"x == "deviceinfos"x ]; then
	#设备信息-1:4:device?${device_id}-ssdb hash
	$bin -h $ssdb_host -p $ssdb_port hlist "${deviceinfo}" "${deviceinfo}${search_postfix}" $INT_MAX
elif [ "$1"x == "deviceinfo"x ]; then
	if [ $# -lt 2 ]; then
		echo "deviceinfo deviceid" && exit 0
	fi
	$bin -h $ssdb_host -p $ssdb_port hgetall ${deviceinfo}"?"${2}
else
	echo "wrong cmd.option:[$cmds]" && exit 1
	exit 0
fi


endtime=`date +'%Y-%m-%d %H:%M:%S'`
start_seconds=$(date --date="$starttime" +%s);
end_seconds=$(date --date="$endtime" +%s);
echo "run time:"$((end_seconds-start_seconds))"s"
