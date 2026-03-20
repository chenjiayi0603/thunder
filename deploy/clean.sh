#!/bin/bash 
#清除文件脚本
RUN_PATH=`pwd`
cd ${RUN_PATH}
config_file="server_dir.conf"
log="/log"

function Usage()
{
	echo -e "configure ${config_file} before running (see deploy/server_dir.conf)"
    echo -e "Usage: $0 [OPTION]..."
    echo -e "options: ./`basename $0` [-h|--help/all/plugin/log/bin]"
}

if [ $# -lt 1 ]; then 
    Usage
	exit 1; 
fi

function CleanPlugin()
{
	while read nodetype plugin  others
	do
	    set -x `find ${RUN_PATH}${plugin} -type f -name "*.so" | xargs -i unlink {}`
	done < ${config_file}
	echo "succ to clean plugin"
}

function CleanLog()
{
	while read nodetype plugin  others
	do
	    set -x `find ${RUN_PATH}/${nodetype}${log} -type f -name '*.log*'| xargs -i unlink {}` 
	done < ${config_file}
	echo "succ to clear log"
}

function CleanBin()
{
	while read nodetype plugin  others
    do
        set -x `find ${RUN_PATH}/${nodetype}/bin -type f -name "*" | xargs -i unlink {}`
    done < ${config_file}
    echo "succ to clear bin"
}

function CleanCore()
{
	find ./ -maxdepth 3 -type f -name "core*"  |xargs -i rm {}
}


while  true :
do
    case "$1" in
        -h|--help)
            Usage
            break
            ;;
        all)
            CleanLog
			CleanPlugin
			CleanBin
			CleanCore
            break
            ;;
        plugins)
            CleanPlugin
            break
            ;;
        log)
            CleanLog
            break
            ;;
        bin)
            CleanBin
            break
            ;;
        core)
            CleanCore
            break
            ;;
        *)
        	echo "nothings to done"
            Usage
            break
            ;;
    esac
done

