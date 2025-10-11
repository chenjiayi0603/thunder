/*
 * LevelDb.cpp
 *
 *  Created on: 2021年1月20日
 *      Author: lm26
 */
#include "LevelDb.hpp"

namespace util
{

CLevelDbi::~CLevelDbi()
{
	if (m_boOpened)
	{
		delete m_db;
		m_db = nullptr;
		m_boOpened = false;
	}
}

bool CLevelDbi::Open(const std::string &dbpath,const std::string &dbname,bool boWriteSync,bool boCreateIfMissing,
		int max_open_files,size_t max_file_size,leveldb::CompressionType compression)
{
	if (!m_boOpened)
	{
		if (dbpath.size() == 0 || dbname.size() == 0)
		{
			return false;
		}
		m_srtDbPath = dbpath;
		m_strDbName = dbname;
		leveldb::Options options;
		options.create_if_missing = boCreateIfMissing;
		options.max_open_files = max_open_files;
		options.max_file_size = max_file_size;
		options.compression = compression;
		//options.error_if_exists = true;
		leveldb::Status s = leveldb::DB::Open(options,GetDbAllPath().c_str(),&m_db);//"./lvd.db"
		if(!s.ok()){
	//		cerr << s.ToString() << endl;
			return false;
		}
		m_WriteOptions = leveldb::WriteOptions();
		m_WriteOptions.sync = boWriteSync;
		m_ReadOptions = leveldb::ReadOptions();
		m_boOpened = true;
	}
	return true;
}


bool CLevelDbi::Put(const std::string& key,const std::string& value)
{
	if (m_boOpened)
	{
		leveldb::WriteBatch batch;
	    batch.Put(key, value);
	    m_LastStatus = m_db->Write(m_WriteOptions, &batch);
		if(!m_LastStatus.ok()){
			m_strLastError = m_LastStatus.ToString();
			return false;
		}
		return true;
	}
	m_LastStatus = leveldb::Status::NotFound("not open db");
	m_strLastError = "not open db";
	return false;
}

bool CLevelDbi::Put(const std::unordered_map<std::string,std::string> &m)
{
	if (m_boOpened)
	{
		leveldb::WriteBatch batch;
		for(const auto& iter:m)
		{
			batch.Put(iter.first, iter.second);
		}
	    m_LastStatus = m_db->Write(m_WriteOptions, &batch);
		if(!m_LastStatus.ok()){
			m_strLastError = m_LastStatus.ToString();
			return false;
		}
		return true;
	}
	m_LastStatus = leveldb::Status::NotFound("not open db");
	m_strLastError = "not open db";
	return false;
}

bool CLevelDbi::Get(const std::string& key,std::string& value)
{
	if (m_boOpened)
	{
		m_LastStatus = m_db->Get(m_ReadOptions,key,&value);
		if(!m_LastStatus.ok()){
			m_strLastError = m_LastStatus.ToString();
			return false;
		}
		return true;
	}
	m_LastStatus = leveldb::Status::NotFound("not open db");
	m_strLastError = "not open db";
	return false;
}

bool CLevelDbi::GetAll(std::unordered_map<std::string,std::string> &m,int iLimit)
{
	if (m_boOpened)
	{
		leveldb::Iterator* it = m_db->NewIterator(m_ReadOptions);
		int i = 0;
		for (it->SeekToFirst(); it->Valid() && i++ < iLimit; it->Next()) {
			m.insert(std::make_pair(std::string(it->key().data(),it->key().size()),std::string(it->value().data(),it->value().size())));
		}
		m_LastStatus = it->status();
		m_strLastError = m_LastStatus.ToString();
		delete it;
		return m_LastStatus.ok();
	}
	m_LastStatus = leveldb::Status::NotFound("not open db");
	m_strLastError = "not open db";
	return false;
}

//`You can also process entries in reverse order. (Caveat: reverse iteration may be
//`somewhat slower than forward iteration.)
bool CLevelDbi::GetAllReverse(std::unordered_map<std::string,std::string> &m,int iLimit)
{
	if (m_boOpened)
	{
		leveldb::Iterator* it = m_db->NewIterator(m_ReadOptions);
		int i = 0;
		for (it->SeekToLast(); it->Valid() && i++ < iLimit; it->Prev()) {
			m.insert(std::make_pair(std::string(it->key().data(),it->key().size()),std::string(it->value().data(),it->value().size())));
		}
		m_LastStatus = it->status();
		m_strLastError = m_LastStatus.ToString();
		delete it;
		return m_LastStatus.ok();
	}
	m_LastStatus = leveldb::Status::NotFound("not open db");
	m_strLastError = "not open db";
	return false;
}

//The following variation shows how to process just the keys in the range
//[start,strEnd):
bool CLevelDbi::GetRange(std::unordered_map<std::string,std::string> &m,const std::string& strStart,const std::string& strEnd,int iLimit)
{
	if (m_boOpened)
	{
		int i = 0;
		leveldb::Iterator* it = m_db->NewIterator(m_ReadOptions);
		for (it->Seek(strStart);it->Valid() && it->key().ToString() < strEnd && i++ < iLimit; it->Next())
		{
			m.insert(std::make_pair(std::string(it->key().data(),it->key().size()),std::string(it->value().data(),it->value().size())));
		}
		m_LastStatus = it->status();
		m_strLastError = m_LastStatus.ToString();
		delete it;
		return m_LastStatus.ok();
	}
	m_LastStatus = leveldb::Status::NotFound("not open db");
	m_strLastError = "not open db";
	return false;
}

bool CLevelDbi::Delete(const std::string& key)
{
	if (m_boOpened)
	{
		leveldb::WriteBatch batch;
		batch.Delete(key);
		m_LastStatus = m_db->Write(m_WriteOptions, &batch);
		if(!m_LastStatus.ok()){
			m_strLastError = m_LastStatus.ToString();
			return false;
		}
		return true;
	}
	m_LastStatus = leveldb::Status::NotFound("not open db");
	m_strLastError = "not open db";
	return false;
}



}
