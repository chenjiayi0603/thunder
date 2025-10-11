/*
 * LevelDb.hpp
 *
 *  Created on: 2021年1月20日
 *      Author: lm26
 */

#ifndef CODE_UTIL_SRC_DBI_LEVELDB_HPP_
#define CODE_UTIL_SRC_DBI_LEVELDB_HPP_

#include <limits.h>
#include <string>
#include <unordered_map>

#include "leveldb/db.h"
#include "leveldb/write_batch.h"

namespace util
{

class CLevelDbi
{
public:
	CLevelDbi() = default;
	~CLevelDbi();

	//param dbpath:目录
	//param dbname:库名
	//param boWriteSync:写同步
	//param boCreateIfMissing:没有库文件也创建
	//param max_open_files:单库最大打开文件数
	//param max_file_size:单文件最大大小（字节）
	//param compression:压缩
	bool Open(const std::string &dbpath,const std::string &dbname,bool boWriteSync = true,bool boCreateIfMissing = true,
			int max_open_files = 1000,size_t max_file_size = 2 * 1024 * 1024,
			leveldb::CompressionType compression = leveldb::kSnappyCompression);

	bool IsOpened()const {return m_boOpened;}

	bool Put(const std::string& key,const std::string& value);

	bool Put(const std::unordered_map<std::string,std::string> &m);

	bool Get(const std::string& key,std::string& value);

	bool GetAll(std::unordered_map<std::string,std::string> &m,int iLimit = INT_MAX);

	bool GetAllReverse(std::unordered_map<std::string,std::string> &m,int iLimit = INT_MAX);

	bool GetRange(std::unordered_map<std::string,std::string> &m,const std::string& strStart,const std::string& strEnd,int iLimit);

	bool Delete(const std::string& key);

	leveldb::Status GetLastStatus()const {return m_LastStatus;}
	const std::string& GetLastError()const {return m_strLastError;}

	const std::string& GetDbPath()const {return m_srtDbPath;}
	const std::string& GetDbName()const {return m_strDbName;}
	const std::string GetDbAllPath()const {return m_srtDbPath + "/" + m_strDbName;}
private:
	std::string m_strLastError;
	leveldb::Status m_LastStatus;

	leveldb::WriteOptions m_WriteOptions;
	leveldb::ReadOptions m_ReadOptions;

	bool m_boOpened = false;
	leveldb::DB *m_db = nullptr;

	std::string m_srtDbPath;
	std::string m_strDbName;

};


} /* namespace util */


#endif /* CODE_UTIL_SRC_DBI_LEVELDB_HPP_ */
