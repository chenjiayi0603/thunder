##  第三方库版本
cryptopp 5.62
curl-7.61.0
c-ares-1.14.0
jemalloc-5.1.0
log4cplus-2.0.2
protobuf-3.6.1
libev 4.22
hiredis-vip 1.0.0
leveldb-1.23
mariadb-connector-c 3.4.7
mongo-c-driver 2.1.2

## 安装第三方库
如果项目目录为/app/thunder
第三方库目录为 /app/thunder/deploy/3lib

文件如下：

libbson2.so        libcryptopp.so  libhiredis.so.0.11     libleveldb.so.1            libmariadb.so.3      libprotobuf.so.17.0.0
libbson2.so.2      libcurl.so      libhiredis_vip.so      libleveldb.so.1.23.0       libmongoc2.so        libprotoc.so
libbson2.so.2.1.2  libev.so        libhiredis_vip.so.1.0  liblog4cplus-1.2.so.5      libmongoc2.so.2      libprotoc.so.17
libcares.so        libev.so.4      libjemalloc.so         liblog4cplus-1.2.so.5.1.6  libmongoc2.so.2.1.2  libprotoc.so.17.0.0
libcares.so.2      libev.so.4.0.0  libjemalloc.so.2       liblog4cplus.so            libprotobuf.so       protoc
libcares.so.2.2.0  libhiredis.so   libleveldb.so          libmariadb.so              libprotobuf.so.17    readme.txt

##  第三方库安装步骤如下
### 安装mongo-c
git clone https://github.com/mongodb/mongo-c-driver.git
cd mongo-c-driver
git checkout 2.1.2   
cd build 
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/mongo-c-driver-2.1.2 ..
make

### 安装libev
git clone https://github.com/enki/libev.git
cd libev
./configure
make

### 安装hiredis-vip
wget https://github.com/vipshop/hiredis-vip/archive/refs/tags/v1.0.0.tar.gz
tar -xzvf v1.0.0.tar.gz
cd hiredis-vip-1.0.0
make

###  安装jemalloc
wget https://github.com/jemalloc/jemalloc/releases/download/5.1.0/jemalloc-5.1.0.tar.bz2
tar -xjvf jemalloc-5.1.0.tar.bz2
cd jemalloc-5.1.0
./configure
make

### c-ares
wget https://c-ares.org/download/c-ares-1.14.0.tar.gz
tar -xzvf c-ares-1.14.0.tar.gz
cd c-ares-1.14.0
./configure
make

###  安装curl
wget https://curl.se/download/curl-7.61.0.tar.gz  
tar -xzvf curl-7.61.0.tar.gz   
cd curl-7.61.0
make

###  安装mariadb客户端库
wget https://github.com/mariadb-corporation/mariadb-connector-c/archive/refs/tags/v3.4.7.tar.gz
tar xvf v3.4.7.tar.gz 
cd mariadb-connector-c
mkdir build && cd build
cmake .. -DOPENSSL_ROOT_DIR=/usr/local/ssl  # 指定 OpenSSL 路径
make


###  安装protobuf库
wget https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.6.1.tar.gz
sudo apt-get install autoconf automake libtool pkg-config
./autogen.sh
./configure CXXFLAGS="-std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1" 
make

###  安装log4cplus
wget https://github.com/log4cplus/log4cplus/releases/download/REL_1_2_1/log4cplus-1.2.1.tar.gz
tar -xvf log4cplus-1.2.1.tar.gz
cd llog4cplus-1.2.1
./configure
make

###  安装CRYPTOPP
wget https://github.com/weidai11/cryptopp/archive/refs/tags/CRYPTOPP_5_6_2.tar.gz
tar -xzf CRYPTOPP_5_6_2.tar.gz
cd cryptopp-CRYPTOPP_5_6_2
make dynamic CXXFLAGS="-Wno-narrowing -fPIC"

###  安装leveldb
git clone --recurse-submodules https://github.com/google/leveldb.git
git fetch --all --tags
git checkout -f 1.23

vim  CMakeLists.txt
添加
set(BUILD_SHARED_LIBS 1)

mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .

