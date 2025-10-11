#include <iostream>
#include <string>
#include <sys/time.h>
#include "leveldb/db.h"

using namespace std;
using namespace leveldb;

#define TEST_DATA_NUM 100000
#define TEST_DATA_LEN 100
#define MAX_LEN 10000

struct timeval start,current;

#include "leveldb/write_batch.h"

//kOk = 0,
//kNotFound = 1,
//kCorruption = 2,
//kNotSupported = 3,
//kInvalidArgument = 4,
//kIOError = 5

void test1()
{
	DB *db;
	Options options;
	options.create_if_missing = true;
	//options.error_if_exists = true;
	Status s = DB::Open(options,"./lvd.db",&db);//数据目录
	if(!s.ok()){
		cerr << s.ToString() << endl;exit(-1);
	}

	std::string value = "value1";
	std::string key1 = "key1";
	std::string key2 = "key2";
	s = db->Get(leveldb::ReadOptions(), key1, &value);
	if (s.ok()) {
	  leveldb::WriteBatch batch;
	  batch.Delete(key1);
	  batch.Put(key1, value);
	  s = db->Write(leveldb::WriteOptions(), &batch);
	  if (s.ok())
	  {
		  cout << "1 ok" << endl;
	  }
	}
	else//NotFound:
	{
		leveldb::WriteBatch batch;
		batch.Put(key1, value);
		batch.Delete(key1);
		s = db->Write(leveldb::WriteOptions(), &batch);
		if (s.ok())
		{
		  cout << "2 ok" << endl;
		}
	}
	delete db;
}

void test11()
{
	DB *db;
	Options options;
	options.create_if_missing = true;
	//options.error_if_exists = true;
	Status s = DB::Open(options,"./lvd.test11.db",&db);//数据目录
	if(!s.ok()){
		cerr << s.ToString() << endl;exit(-1);
	}
	WriteOptions oWriteOptions = leveldb::WriteOptions();
	oWriteOptions.sync = true;

	leveldb::WriteBatch batch;
	for(int i = 0;i < 10000;++i)
	{
		batch.Put(std::string("key") + std::to_string(i), std::string("val") + std::to_string(i));
	}
	s = db->Write(oWriteOptions, &batch);
	if (s.ok())
	{
	  cout << "2 ok" << endl;
	}
	{
		//The following example demonstrates how to print all key,value pairs in a
		//database.
		int counter(0);
		cout << "SeekToFirst" << endl;
		leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
		for (it->SeekToFirst(); it->Valid(); it->Next()) {
//		  cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
		  ++counter;
		}
		assert(it->status().ok());  // Check for any errors found during the scan
		delete it;
		cout << "SeekToFirst total " << counter << endl;
	}
	delete db;
}

void test111()
{
	DB *db;
	Options options;
	options.create_if_missing = true;
	//options.error_if_exists = true;
	Status s = DB::Open(options,"./lvd.test111.db",&db);//数据目录
	if(!s.ok()){
		cerr << s.ToString() << endl;exit(-1);
	}
	WriteOptions oWriteOptions = leveldb::WriteOptions();
	oWriteOptions.sync = true;

	leveldb::WriteBatch batch;
	for(int i = 0;i < 100;++i)
	{
		batch.Put(std::string("key") + std::to_string(i), std::string("val") + std::to_string(i));
	}
	s = db->Write(oWriteOptions, &batch);
	if (s.ok())
	{
	  cout << "2 ok" << endl;
	}
	{
		//The following example demonstrates how to print all key,value pairs in a
		//database.
		int counter(0);
		cout << "SeekToFirst" << endl;
		leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
		for (it->SeekToFirst(); it->Valid(); it->Next()) {
		  cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
		  ++counter;
		}
		assert(it->status().ok());  // Check for any errors found during the scan
		delete it;
		cout << "SeekToFirst total " << counter << endl;
	}
	delete db;
}

void test2()
{
	DB *db;
	Options options;
	options.create_if_missing = true;
	//options.error_if_exists = true;
	Status s = DB::Open(options,"./lvd.db",&db);
	if(!s.ok()){
		cerr << s.ToString() << endl;exit(-1);
	}
	int i;
	char k[100],v[MAX_LEN+1];
	string key,val;
	for(i = 0;i < MAX_LEN && i < TEST_DATA_LEN ;i ++)
	{
		v[i] = 'x';
	}
	v[i] = '\0';
	val = v;

	//write db test
	gettimeofday(&start,NULL);
	for(i = 0;i < TEST_DATA_NUM; i ++){
		snprintf(k,sizeof(k),"key%d",i);
		key = k;
		s = db->Put(WriteOptions(),key,val);
		if(!s.ok()){
			cerr << s.ToString() << endl;exit(-1);
		}
		if(i % 10000 == 0){
			gettimeofday(&current,NULL);
			double microsecs = 1000000*(current.tv_sec-start.tv_sec)+(current.tv_usec-start.tv_usec);
			printf("write db num %d cost time %lf(ms) %lf(ops)\n",i,microsecs,i/(microsecs/1000000));
		}
	}

	//read db test
	gettimeofday(&start,NULL);
	for(i = 0;i < TEST_DATA_NUM; i ++){
		snprintf(k,sizeof(k),"key%d",i);
		key = k;
		s = db->Get(ReadOptions(),key,&val);
		if(!s.ok()){
			cerr << s.ToString() << endl;exit(-1);
		}
		if(i % 10000 == 0){
			gettimeofday(&current,NULL);
			double microsecs = 1000000*(current.tv_sec-start.tv_sec)+(current.tv_usec-start.tv_usec);
			printf("read db num %d cost time %lf(ms) %lf(ops)\n",i,microsecs,i/(microsecs/1000000));
		}
	}
	delete db;
}

void test3()
{
	DB *db;
	Options options;
	options.create_if_missing = true;
	//options.error_if_exists = true;
	Status s = DB::Open(options,"./lvd.db2",&db);//数据目录
	if(!s.ok()){
		cerr << s.ToString() << endl;exit(-1);
	}
	{
		//The following example demonstrates how to print all key,value pairs in a
		//database.
		int counter(0);
		cout << "SeekToFirst" << endl;
		leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
		for (it->SeekToFirst(); it->Valid(); it->Next()) {
		  cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
		  ++counter;
		}
		assert(it->status().ok());  // Check for any errors found during the scan
		delete it;
		cout << "SeekToFirst " << counter << endl;
	}

	//The following variation shows how to process just the keys in the range
	//[start,limit):
	{
		cout << "Seek(\"key2\")" << endl;
		leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
		for (it->Seek("key2");
		   it->Valid() && it->key().ToString() < "key3";
		   it->Next()) {
			cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
		}
		delete it;
	}

	//`You can also process entries in reverse order. (Caveat: reverse iteration may be
	//`somewhat slower than forward iteration.)
	{
		cout << "SeekToLast" << endl;
		leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
		for (it->SeekToLast(); it->Valid(); it->Prev()) {
			cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
		}
		delete it;
	}
	delete db;
}

#pragma pack(1)
struct Data {
int i = 0;
char ch[10];
bool assign(const std::string& str)
{
	if (sizeof(*this) == str.size())
	{
		memcpy(this,str.data(),str.size());
		return true;
	}
	return false;
}
};
#pragma pack()

void test4()
{
	DB *db;
	Options options;
	options.create_if_missing = true;
	//options.error_if_exists = true;
	Status s = DB::Open(options,"./lvd.db2",&db);//数据目录
	if(!s.ok()){
		cerr << s.ToString() << endl;exit(-1);
	}

	{
		Data oData;
		oData.i = 1;
		snprintf(oData.ch,sizeof(oData.ch),"123456");

		Slice oSlice((const char *)&oData,sizeof(oData));
		s = db->Put(WriteOptions(),"bin1",oSlice);
		if(!s.ok()){
			cerr << s.ToString() << endl;exit(-1);
		}
	}
	{
		std::string val;
		s = db->Get(ReadOptions(),"bin1",&val);
		if(!s.ok()){
			cerr << s.ToString() << endl;exit(-1);
		}
		Data oData;
		if (oData.assign(val))
		{
			cout << "i:"<< oData.i << "  ch:" << oData.ch << endl;
		}
		else
		{
			cout << "assign failed" << endl;
		}
	}
	delete db;
}

void test5()
{
	DB *db;
	Options options;
	options.create_if_missing = true;
	//options.error_if_exists = true;
	Status s = DB::Open(options,"./lvd.db2",&db);//数据目录
	if(!s.ok()){
		cerr << s.ToString() << endl;exit(-1);
	}
	WriteOptions oWriteOptions = WriteOptions();
	oWriteOptions.sync = true;
	{
		Data oData;
		oData.i = 1;
		snprintf(oData.ch,sizeof(oData.ch),"123456");

		std::string oStr((const char *)&oData,sizeof(oData));
		s = db->Put(oWriteOptions,std::string("test5:str11"),oStr);
		if(!s.ok()){
			cerr << s.ToString() << endl;exit(-1);
		}
	}
	{
		std::string val;
		s = db->Get(ReadOptions(),std::string("test5:str11"),&val);
		if(!s.ok()){
			cerr << s.ToString() << endl;exit(-1);
		}
		Data oData;
		if (oData.assign(val))
		{
			cout << __FUNCTION__ << " test5:str11 i:"<< oData.i << "  ch:" << oData.ch << endl;
		}
		else
		{
			cout << __FUNCTION__ << "assign failed" << endl;
		}
	}
	delete db;
}

//int main()
//{
////	test11();
////	test3();
////	test5();
//	test111();
//	return 0;
//}
/*
./test_leveldb
write db num 0 cost time 22.000000(ms) 0.000000(ops)
write db num 10000 cost time 28317.000000(ms) 353144.754035(ops)
write db num 20000 cost time 57786.000000(ms) 346104.592808(ops)
write db num 30000 cost time 87247.000000(ms) 343851.364517(ops)
write db num 40000 cost time 115175.000000(ms) 347297.590623(ops)
write db num 50000 cost time 143165.000000(ms) 349247.371914(ops)
write db num 60000 cost time 171008.000000(ms) 350860.778443(ops)
write db num 70000 cost time 199044.000000(ms) 351681.035349(ops)
write db num 80000 cost time 226929.000000(ms) 352533.171168(ops)
write db num 90000 cost time 254765.000000(ms) 353266.736012(ops)
read db num 0 cost time 8.000000(ms) 0.000000(ops)
read db num 10000 cost time 40029.000000(ms) 249818.881311(ops)
read db num 20000 cost time 63218.000000(ms) 316365.592078(ops)
read db num 30000 cost time 86832.000000(ms) 345494.748480(ops)
read db num 40000 cost time 108950.000000(ms) 367140.890317(ops)
read db num 50000 cost time 130858.000000(ms) 382093.567073(ops)
read db num 60000 cost time 152579.000000(ms) 393238.912301(ops)
read db num 70000 cost time 175278.000000(ms) 399365.579251(ops)
read db num 80000 cost time 197723.000000(ms) 404606.444369(ops)
read db num 90000 cost time 218517.000000(ms) 411867.268908(ops)
 * */
