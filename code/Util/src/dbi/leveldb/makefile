all:
	#export LD_LIBRARY_PATH=/app/leveldb/build 
	g++ test_leveldb.cpp -o test_leveldb -lleveldb -lpthread -I/app/leveldb/include -L/app/leveldb/build -std=c++11
	#git clone --recurse-submodules https://github.com/google/leveldb.git
	#mkdir -p build && cd build
	#cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .