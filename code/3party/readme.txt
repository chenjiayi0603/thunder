第三方库版本
cryptopp 5.62
curl-7.61.0
c-ares-1.14.0
jemalloc-5.1.0
log4cplus-2.0.2
protobuf-3.6.1
libev-master(4.22)
hiredis-vip-master(0.3)


如果动态库找不到函数符号，则重新编译动态库并替换

安装curl
wget https://curl.se/download/curl-7.61.0.tar.gz  
tar -xzvf curl-7.61.0.tar.gz   
cd curl-7.61.0
make

安装mariadb客户端库
wget https://github.com/mariadb-corporation/mariadb-connector-c/archive/refs/tags/v3.4.7.tar.gz
tar xvf v3.4.7.tar.gz 
cd mariadb-connector-c
mkdir build && cd build
cmake .. -DOPENSSL_ROOT_DIR=/usr/local/ssl  # 指定 OpenSSL 路径
make


安装protobuf库
wget https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.6.1.tar.gz
sudo apt-get install autoconf automake libtool pkg-config
./autogen.sh
./configure CXXFLAGS="-std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1" 
make

安装log4cplus
wget https://github.com/log4cplus/log4cplus/releases/download/REL_1_2_1/log4cplus-1.2.1.tar.gz
tar -xvf log4cplus-1.2.1.tar.gz
cd llog4cplus-1.2.1
./configure
make

安装CRYPTOPP
wget https://github.com/weidai11/cryptopp/archive/refs/tags/CRYPTOPP_5_6_2.tar.gz
tar -xzf CRYPTOPP_5_6_2.tar.gz
cd cryptopp-CRYPTOPP_5_6_2
make dynamic CXXFLAGS="-Wno-narrowing -fPIC"



