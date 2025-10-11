#!/bin/bash 
RUN_PATH=`pwd`
cd ${RUN_PATH}
src_path="/lib"
dest_path="/lib"
config_file="server_dir.conf"
server_file="server_list.conf"

proto_so=libProto.so
lib_so=libUtil.so
net_so=libNet.so

function Usage()
{
	echo -e "configure server_dir.conf  and  server_list.conf  before installation"
    echo -e "Usage: ./`basename $0` [OPTION1] [OPTION2]"
    echo -e "OPTION1: [-h|--help/all/install/clean]"
    echo -e "OPTION2: ["
    while read server others
    do 
    	echo "/${server}"
    done < ${server_file}
    echo -e "]"
}

if [ $# -lt 1 ]; then 
    Usage
	exit 1; 
fi


function PrintSo()
{
    while read nodetype others
    do 
    	if [[ "$1"x == ""x ]] || [[ "$1"x == "${nodetype}"x ]] ; then
    		echo "${nodetype}:"
	        echo "print so src"
	        find  ${RUN_PATH}${src_path} -type f -name "*.so" | xargs -i ls -l --color=tty {}
	        echo "print so dest"
	        find  ${RUN_PATH}/${nodetype}${dest_path} -type f -name "*.so" | xargs -i ls -l --color=tty {}
    	fi
    done < ${config_file}
}

function Install()
{
	cd ${RUN_PATH}
    while read nodetype others
    do
    	if [[ "$1"x == ""x ]] || [[ "$1"x == "${nodetype}"x ]] ; then
    		echo "install ${proto_so}"
			test -d ${RUN_PATH}/${nodetype}${dest_path} && install -vD ${RUN_PATH}${src_path}/${proto_so}  ${RUN_PATH}/${nodetype}${dest_path}
		    echo "install ${lib_so}"
		    test -d ${RUN_PATH}/${nodetype}${dest_path} && install -vD ${RUN_PATH}${src_path}/${lib_so}  ${RUN_PATH}/${nodetype}${dest_path}
		    echo "install ${net_so}"
		    test -d ${RUN_PATH}/${nodetype}${dest_path} && install -vD ${RUN_PATH}${src_path}/${net_so}  ${RUN_PATH}/${nodetype}${dest_path}	
    	fi
    done < ${config_file}
	PrintSo $1
}

function Clean()
{
	cd ${RUN_PATH}
    while read nodetype others
    do
    	echo "echo $1 ${nodetype}"
    	if [[ "$1"x == ""x ]] || [[ "$1"x == "${nodetype}"x ]] ; then
    		echo "unlink ${proto_so}"
	    	unlink ${RUN_PATH}/${nodetype}${dest_path}/${proto_so}
	    	echo "unlink ${lib_so}"
	    	unlink ${RUN_PATH}/${nodetype}${dest_path}/${lib_so}
	    	echo "unlink ${net_so}"
	    	unlink ${RUN_PATH}/${nodetype}${dest_path}/${net_so}
    	fi
    done < ${config_file}
	PrintSo $1
}

while  true :
do
    case "$1" in
        -h|--help)
            Usage
            break
            ;;
        all)
            Install  
            break
            ;;
        install)
            Install "$2"
            break
            ;;
        clean)
            Clean "$2"
            break
            ;;
        *)
        	Install "$1"
            break
            ;;
    esac
done
 


