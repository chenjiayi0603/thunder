#!/bin/bash
#编译代码
code_path=`pwd`
cd ${code_path}
run_path=${code_path}/../deploy
lib3_path=/app/analysis/analysisServer/deploy/3lib
code_lib3_path=${code_path}/l3lib/lib

if [ $# -lt 1 ]; then 
    echo "USAGE: $0 param1" 
    echo "please input option[install,all,pre]"
	exit 1; 
fi

if [ "$1"x == "compile"x ];then  
	#清除
	cd ${run_path}
	./clear.sh all
	#编译
	cd ./core && ./make_libs.sh all
	cd ${code_path} && ./logic_proto.sh all && ./logic_plugins.sh all
elif [ "$1"x == "install"x ];then 
	#清除
	cd ${run_path}
	./clear.sh all
	#安装
	cd ${run_path}
	./install all
elif [ "$1"x == "core"x ];then 
	cd ./core &&  ./make_core.sh all && cd ..
elif [ "$1"x == "all"x ];then 
	#拷贝makefile模板到逻辑节点，使用core服务器库需要使用指定统一makefile模板
	./core/makefiles/makefilecopy.sh
	#编译
	cd ./core &&  ./make_core.sh all && cd ..
	cd ${code_path} && ./logic_proto.sh all && ./logic_plugins.sh all
elif [ "$1"x == "pre"x ];then
	#准备工作
	find ./ -maxdepth 3 -type f -name "*.sh"  |xargs -i chmod +x {}
	#test ! -d ${code_lib3_path} && test -d ${lib3_path} && ln -s ${lib3_path} ${code_lib3_path} && echo "ln 3lib for code"
	#test ! -d ${run_path}/bin && mkdir ${run_path}/bin
	#test ! -d ${run_path}/lib && mkdir ${run_path}/lib
	#test ! -d ${run_path}/plugins && mkdir ${run_path}/plugins
	#cd ${run_path} 
	#chmod +x ./install.sh && ./install.sh pre && echo "install.sh pre ok"
	#chmod +x ../tools/bin/*
else
	echo "do nothings"
fi
