#!/bin/bash
ANALYSIS_HOME=`dirname $0`
SCRIPT_NAME=`basename $0`
cd ${ANALYSIS_HOME}
ANALYSIS_HOME=`pwd`
RUN_PATH=`pwd`
log=data/file
pack=data/pack
data=data
db_ip=192.168.18.78
db_port=9200

. ${ANALYSIS_HOME}/scripts/analysis_script_func.sh

cd ${RUN_PATH}

if [ $# -lt 1 ]; then 
    echo "USAGE: $0 param1" 
    echo "please input option[fileall,filenew,searchfile param2,pack,unpack,addtasks,map,upload,uploadall,overdue]"
	exit 1; 
fi

starttime=`date +'%Y-%m-%d %H:%M:%S'`
PROGRAM=${RUN_PATH}/upload_es.sh

test ! -d ${RUN_PATH}/${data} && mkdir -p ${RUN_PATH}/${data}
last_upload=`test -f ${data}/last_upload.txt && cat ${data}/last_upload.txt|| echo ""`
echo "last_upload:${last_upload}"

last_pack=`test -f ${data}/last_pack.txt && cat ${data}/last_pack.txt|| echo ""`
echo "last_pack:${last_pack}"

function upload()
{
	test ! -d ${RUN_PATH}/${log} && mkdir -p ${RUN_PATH}/${log}
	filelist=`ls ${RUN_PATH}/${log}`
	for filename in $filelist ; do
	    if [ -f ${RUN_PATH}/${log}/${filename} ] && [ "${filename##*.}"x = "dat"x ]; then 
	        #没被占用的文件才能提交分析
	        pid=`lsof ${RUN_PATH}/${log}/${filename} | awk 'END{print $2}'`
	        if [ -z "$pid" ];then
	            filesize=`ls -ld ${RUN_PATH}/${log}/${filename} | awk '{print int($5)}'`
	            #echo "filename:${filename} last_upload:${last_upload}"
	            if [ $filesize != 0 ];then
		            if [ "${filename}"L \> "${last_upload}"L ];then
		            	cmd="curl  -H 'Content-Type: application/json;charset=UTF-8' -XPOST '${db_ip}:${db_port}/_bulk?pretty' --data-binary @${RUN_PATH}/${log}/${filename}"
		            	echo $cmd
		            	eval $cmd >/dev/null 2>&1
		            	if [ $? -eq 0 ];then 
		            		echo "succ to upload ${RUN_PATH}/${log}/${filename}" 
		            	fi
		            	echo ${filename} > ${data}/last_upload.txt
		            	last_upload=`cat ${data}/last_upload.txt`
		        	fi
		        else
		        	warn_log "del empty file:${RUN_PATH}/${log}/${filename}"
		        	rm ${RUN_PATH}/${log}/${filename}
	            fi
	        fi
		else
	        debug_log "File ${RUN_PATH}/${log}/${filename} is not a dat file."
	    fi 
	done
	
	cmd="curl '${db_ip}:${db_port}/_cat/indices?v'"
	echo $cmd
	eval $cmd
	
	#curl '192.168.18.78:9200/db_trace/_search?pretty'
	#curl '192.168.18.78:9200/db_trace/tb_trace/_search?pretty'
}

#仅做测试使用
function modify_userid()
{
	test ! -d ${RUN_PATH}/${log} && mkdir -p ${RUN_PATH}/${log}
	filelist=`ls ${RUN_PATH}/${log}`
	for filename in $filelist ; do
	    if [ -f ${RUN_PATH}/${log}/${filename} ] && [ "${filename##*.}"x = "dat"x ]; then 
	        #没被占用的文件才能提交分析
	        pid=`lsof ${RUN_PATH}/${log}/${filename} | awk 'END{print $2}'`
	        if [ -z "$pid" ];then
	            filesize=`ls -ld ${RUN_PATH}/${log}/${filename} | awk '{print int($5)}'`
	            if [ $filesize != 0 ];then
	            	while read line
					do 
					    device_id=`echo ${line} | jq .device_id` 
					    if [[ "$device_id" =~ ^null.* ]]; then
					    	echo "" > /dev/null 2>&1
					    else
					    	echo "device_id:$device_id"
					    	echo "${RUN_PATH}/${log}/${filename}" |xargs sed -i "s/\"user_id\":\"3d4cc6be-a65d-4bd1-ac75-fcd61100b461\"/\"user_id\":${device_id}/g" > /dev/null 2>&1
							break
						fi
					done < ${RUN_PATH}/${log}/${filename}
		        else
		        	warn_log "del empty file:${RUN_PATH}/${log}/${filename}"
		        	rm ${RUN_PATH}/${log}/${filename}
	            fi
	        fi
		else
	        debug_log "File ${RUN_PATH}/${log}/${filename} is not a dat file."
	    fi 
	done
	
	cmd="curl '${db_ip}:${db_port}/_cat/indices?v'"
	echo $cmd
	eval $cmd
	
	#curl '192.168.18.78:9200/db_trace/_search?pretty'
	#curl '192.168.18.78:9200/db_trace/tb_trace/_search?pretty'
}

function del_tables()
{
	curl  -H 'Content-Type: application/json;charset=UTF-8' -XDELETE ${db_ip}:${db_port}/db_trace/tb_trace/_delete_by_query -d '{"query": {"match_all": {} }}'	
	curl  -H 'Content-Type: application/json;charset=UTF-8' -XDELETE ${db_ip}:${db_port}/db_user/tb_user/_delete_by_query -d '{"query": {"match_all": {} }}'	
	curl  -H 'Content-Type: application/json;charset=UTF-8' -XDELETE ${db_ip}:${db_port}/db_device/tb_device/_delete_by_query -d '{"query": {"match_all": {} }}'	
}

function pack()
{
	if [ -z "${last_upload}" ];then
		echo "wait for upload to pack" && exit 0
	fi
	#每日一个压缩文件
	DATE=$(date +%Y%m%d)
	test ! -d ${RUN_PATH}/${pack} && mkdir -p ${RUN_PATH}/${pack} && echo "mk dir ${RUN_PATH}/${pack}"
	filelist=`ls ${RUN_PATH}/${log}`
	for filename in $filelist ; do
	    if [ -f ${RUN_PATH}/${log}/${filename} ] && [ "${filename##*.}"x = "dat"x ]; then 
	    	#没被占用的文件才能提交压缩
	    	pid=`lsof ${RUN_PATH}/${log}/${filename} | awk 'END{print $2}'`
	        if [ -z "$pid" ];then
	        	filesize=`ls -ld ${RUN_PATH}/${log}/${filename} | awk '{print int($5)}'`
	            #echo "filename:${filename} last_upload:${last_upload}"
	            if [ $filesize != 0 ];then
	            	#压缩已提交的
		            if [ "${filename}"L \< "${last_upload}"L ] || [ "${filename}"L == "${last_upload}"L ];then
		            	#压缩没压缩过的
		            	if [ "${filename}"L \> "${last_pack}"L ];then
		            		zip ${RUN_PATH}/${pack}/Log_${DATE}.zip  ${log}/${filename}
			            	if [ $? -eq 0 ];then 
			            		echo "succ to pack ${log}/${filename}" 
			            	else 
			            		echo "fail to pack ${log}/${filename}" 
			            	fi
			            	echo ${filename} > ${data}/last_pack.txt
		            		last_pack=`cat ${data}/last_pack.txt`
		            	fi
		        	fi
		        else
		        	warn_log "del empty file:${RUN_PATH}/${log}/${filename}"
		        	rm ${RUN_PATH}/${log}/${filename}
	            fi
	        fi
	    fi
	done
}

function unpack()
{
	test ! -d ${RUN_PATH}/${pack} && mkdir -p ${RUN_PATH}/${pack} && echo "mk dir ${RUN_PATH}/${pack}"
	filelist=`ls ${RUN_PATH}/${pack}`
	for filename in $filelist ; do
	    if [ -f ${RUN_PATH}/${pack}/${filename} ] && [ "${filename##*.}"x = "zip"x ]; then 
            #echo "filename:${filename} last_upload:${last_upload}"
            #已有的不覆盖，没有的就创建
        	unzip -n ${pack}/${filename}   
        	if [ $? -eq 0 ];then 
        		echo "succ to unpack ${pack}/${filename}" 
        	else
        		echo "fail to unpack ${pack}/${filename}" 
        	fi
	    fi
	done
}

function addtaskpack()
{
	COUNT=`crontab -l | grep "$PROGRAM pack" | grep -v "grep"|wc -l ` 
	if [ $COUNT -ge 1 ]; then 
        echo "replace old crontab task" && crontab -l
	fi 
	#每天24:00执行
	CRONTAB_CMD="0 0 * * * sh $PROGRAM pack >> /var/log/crontab_task_pack.log 2>&1" 
	(crontab -l 2>/dev/null | grep -Fv "$PROGRAM pack"; echo "$CRONTAB_CMD") | crontab - 
	COUNT=`crontab -l | grep "$PROGRAM pack" | grep -v "grep"|wc -l ` 
	if [ $COUNT -lt 1 ]; then 
        echo "fail to add crontab $PROGRAM" 
        exit 1 
	fi 
	echo "set crontab task succ" && crontab -l
}

function addtaskupload()
{
	COUNT=`crontab -l | grep "$PROGRAM upload" | grep -v "grep"|wc -l ` 
	if [ $COUNT -ge 1 ]; then 
        echo "replace old crontab task" && crontab -l
	fi 
	#每10分钟执行一次
	CRONTAB_CMD="*/10 * * * * sh $PROGRAM upload >> /var/log/crontab_task_upload.log 2>&1" 
	(crontab -l 2>/dev/null | grep -Fv "$PROGRAM upload"; echo "$CRONTAB_CMD") | crontab - 
	COUNT=`crontab -l | grep "$PROGRAM upload" | grep -v "grep"|wc -l ` 
	if [ $COUNT -lt 1 ]; then 
        echo "fail to add crontab $PROGRAM" 
        exit 1 
	fi 
	echo "set crontab task succ" && crontab -l
}

function searchfile()
{
	echo "search in ${RUN_PATH}/${log} for $1"
	find ${RUN_PATH}/${log} -maxdepth 2  -type f -name "*.dat"|xargs grep -rin --color "$1"
}

function del_overdue()
{
	index_name=$1  
	type_name=$2  
	save_days=$3
	if [ -z "$save_days" ]; then  
	   save_days=90
	fi  
	format_day='%Y-%m-%d' 
	#overdue_day="2018-05-28"
	overdue_day=`date -d "-${save_days} day " +${format_day}`  
	
	curl  -H 'Content-Type: application/json;charset=UTF-8' -XPOST ${db_ip}:${db_port}/${index_name}/${type_name}/_delete_by_query?pretty -d '
	{  
        "query": {  
                "bool": {  
                        "must": {  
                                "range": {  
                                        "date": {  
                                                "from": null,  
                                                "to": "'${overdue_day}'",  
                                                "include_lower": true,  
                                                "include_upper": true  
                                        }  
                                }  
                        }  
                }  
        }  
	}'
	if [ $? -eq 0 ];then 
		echo "succ to del" 
	else
		echo "fail to del" 
	fi
	
	cmd="curl '${db_ip}:${db_port}/_cat/indices?v'"
	echo $cmd
	eval $cmd
}

function update_userid_by_deviceid()
{
	if [ -z "$1" ]; then  
	   echo "empty index_name"&& exit 0
	fi 
	if [ -z "$2" ]; then  
	   echo "empty type_name"&& exit 0
	fi
	index_name=$1  
	type_name=$2  
    curl  -H 'Content-Type: application/json;charset=UTF-8' -XPOST ${db_ip}:${db_port}/${index_name}/${type_name}/_update_by_query?pretty -d '
    {
    	"query": {
	        "term": {
	            "user_id.keyword": ""
	        }
	    },
      	"script" : 
      	{
      		"inline": "ctx._source.user_id = ctx._source.device_id"
      	}
    }'
    if [ $? -eq 0 ];then 
		echo "succ to update_userid_by_deviceid" 
	else
		echo "fail to update_userid_by_deviceid" 
	fi
}

function mapping()
{
	if [ -n "$1" ]; then  
	   curl  -H 'Content-Type: application/json;charset=UTF-8' -XGET "http://${db_ip}:${db_port}/$1/_mapping?pretty" 2>>/dev/null
	   exit 0
	fi  
	curl  -H 'Content-Type: application/json;charset=UTF-8' -XGET "http://${db_ip}:${db_port}/db_trace/_mapping?pretty" 
	curl  -H 'Content-Type: application/json;charset=UTF-8' -XGET "http://${db_ip}:${db_port}/db_user/_mapping?pretty" 
	curl  -H 'Content-Type: application/json;charset=UTF-8' -XGET "http://${db_ip}:${db_port}/db_device/_mapping?pretty" 
}

function fileall()
{
	filenum=`ls -l ${RUN_PATH}${log}/*.dat|grep "^-"| wc -l`
	filelist=`ls ${RUN_PATH}${log}/*.dat`
	wc -l $filelist
	echo "filenum $filenum"
	
	du -ah --max-depth=1 ${RUN_PATH}/data
}

function filenew()
{
	filelist=`ls -t ${RUN_PATH}${log}`
	for filename in $filelist ; do
		if [ -f ${RUN_PATH}${log}/${filename} ] ; then 
	        pid=`lsof ${RUN_PATH}${log}/${filename} | awk 'END{print $2}'`
	        #echo "${RUN_PATH}${log}/${filename} pid $pid"
	        if [ ! -z "$pid" ];then
	        	echo "${RUN_PATH}${log}/${filename} in use for pid $pid"
	        	tail -f ${RUN_PATH}${log}/${filename}
	        fi
	   	fi
	done
}

#文档管理
#检查文档
if [ "$1"x == "fileall"x ];then
	fileall
elif [ "$1"x == "filenew"x ];then
	filenew
elif [ "$1"x == "searchfile"x ];then
	#查找包含指定字段的文档
	if [ $# -lt 2 ]; then 
		echo "search param2"
		exit 1; 
	fi
	searchfile $2
#打包、解包文档
elif [ "$1"x == "pack"x ];then
	pack
elif [ "$1"x == "unpack"x ];then
	unpack	
#定时任务
elif [ "$1"x == "addtasks"x ];then
	#使用root用户来执行,su 加密码
	addtaskupload
	addtaskpack
	crontab -l
#查看模板
elif [ "$1"x == "map"x ];then
	mapping $2
#文档提交es
elif [ "$1"x == "upload"x ];then
	upload
elif [ "$1"x == "uploadall"x ];then
	rm ${data}/last_upload.txt
	last_upload=""
	upload
elif [ "$1"x == "overdue"x ];then	
	#删除es中过期时间的数据
	#如：db_user_test为测试index
	#del_overdue "db_user_test" "tb_user_test" 7
	del_overdue "db_trace" "tb_trace" 90
#测试使用
elif [ "$1"x == "modify_userid"x ];then
	modify_userid
elif [ "$1"x == "update_userid"x ];then	
	update_userid_by_deviceid "db_trace" "tb_trace"
else
	echo "USAGE: $0 param1" 
	echo "please input option[fileall,filenew,searchfile param2,pack,unpack,addtasks,map,upload,uploadall,overdue]"
	exit 1
fi

endtime=`date +'%Y-%m-%d %H:%M:%S'`
start_seconds=$(date --date="$starttime" +%s);
end_seconds=$(date --date="$endtime" +%s);
echo "run time:"$((end_seconds-start_seconds))"s"

