all:
	#libpq.so 是gpdb编译的 ,libpqxx.a 是pqxx编译的
	g++ connection.cpp -o connection -I.. -L../../../lib -lpqxx -lpq -std=c++11
	g++ create.cpp -o create -I.. -L../../../lib -lpqxx -lpq -std=c++11
	g++ insert.cpp -o insert -I.. -L../../../lib -lpqxx -lpq -std=c++11
	g++ query.cpp -o query -I.. -L../../../lib -lpqxx -lpq -std=c++11
	g++ update.cpp -o update -I.. -L../../../lib -lpqxx -lpq -std=c++11
	