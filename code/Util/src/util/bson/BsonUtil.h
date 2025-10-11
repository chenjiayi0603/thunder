/*
 * BsonUtil.h
 *
 *  Created on: 2020年1月7日
 *      Author: Tommy
 */

#ifndef UTIL_BSONUTIL_H_
#define UTIL_BSONUTIL_H_
#include <mongoc/src/libbson/src/bson/bson.h>
#include "util/json/CJsonObject.hpp"

namespace util
{
#define SAFE_DESTROY_BSON(doc) if (doc) {bson_destroy (doc);doc = nullptr;}

//创建单key文档
bson_t *GetBsonDoc(const char* field,bson_oid_t &oid);
bson_t *GetBsonDoc(const char* field,int value);
bson_t *GetBsonDoc(const char* field,const char* value);
bson_t *GetBsonDocInt64(const char* field,long long value);
bson_t *GetBsonDocDouble(const char* field,double value);
bson_t *GetBsonDocBool(const char* field,bool value);

//由json获取Bson对象
bson_t *GetBsonDoc(const util::CJsonObject &jObj,bson_error_t &error);
bson_t *GetBsonDoc(const std::string &jJsonObjStr,bson_error_t &error);


class CBsonObject
{
public:
	CBsonObject() {m_bson = bson_new ();}
	explicit CBsonObject(bson_t* bson):m_bson(bson) {}
	explicit CBsonObject(const util::CBsonObject &jObj)
	{
		Parse(jObj.GetBson());
	}
	explicit CBsonObject(const util::CJsonObject &jObj)
	{
	    const std::string jObjStr = jObj.ToString();
	    m_bson = bson_new_from_json((const unsigned char*) jObjStr.c_str(),(uint32_t) jObjStr.length(), &m_error);
	}
	explicit CBsonObject(const std::string& jObjStr)
	{
		m_bson = bson_new_from_json((const unsigned char*) jObjStr.c_str(),(uint32_t) jObjStr.length(), &m_error);
	}
	virtual ~CBsonObject() {SAFE_DESTROY_BSON (m_bson);}
	CBsonObject& operator=(const CBsonObject& oJsonObject);
	bool operator==(const CBsonObject& oJsonObject) const;
	bool Parse(const std::string& strJson);
	bool Parse(const bson_t* bson);
	std::string ToString() const;
	bool IsEmpty() const;
	void Clear();

	//添加新key到Doc
	void Add(const std::string& strKey,const CBsonObject& object);
	void Add(const std::string& strKey,int value);
	void Add(const std::string& strKey,int64_t value);
	void Add(const std::string& strKey,double value);
	void Add(const std::string& strKey,bool value);
	void Add(const std::string& strKey,const char* value);
	void Add(const std::string& strKey,const std::string& value);//UTF8字符串
	void Add(const std::string& strKey,const bson_t *value);//bson_t对象
	void Add(const std::string& strKey,const uint8_t* value,uint32_t len);//二进制
	void Add(const std::string& strKey,std::vector<int> &value);
	void Add(const std::string& strKey,std::vector<int64_t> &value);
	void Add(const std::string& strKey,std::vector<std::string> &value);
	void AddDataTime(const std::string& strKey,int64_t value);
	void AddTimeStamp(const std::string& strKey,uint32_t value,uint32_t increment = 0);
	void AddBsonValue(const std::string& strKey,const bson_value_t *value);
	void AddCode(const std::string& strKey,const std::string& strCode);
	void AddSymbol(const std::string& strKey,const std::string& strSymbol);
	void AddUndefined(const std::string& strKey);

	bool GetBinary(const std::string& strKey, std::string& strValue) const;
	bool GetArray(const std::string& strKey, std::string& strValue)const;
	bool GetKeys(std::vector<std::string> &keys)const;
	bool Get(const std::string& strKey, std::string& strValue) const;
	bool Get(const std::string& strKey, int32_t& iValue) const;
	bool Get(const std::string& strKey, uint32_t& uiValue) const;
	bool Get(const std::string& strKey, int64_t& llValue) const;
	bool Get(const std::string& strKey, uint64_t& llValue) const;
	bool Get(const std::string& strKey, bool& bValue) const;
	bool Get(const std::string& strKey, float& fValue) const;
	bool Get(const std::string& strKey, double& dValue) const;

	const bson_t *GetBson()const {return m_bson;}
	bool ToJson(util::CJsonObject &jsonObj);
	std::string ToJsonStr();
	const bson_error_t& GetError()const {return m_error;}
private:
	bson_t* m_bson = nullptr;
	bson_error_t m_error;
};

void BsonToJson(const bson_t *bson,util::CJsonObject &jsonObj);

class AutoBson
{
public:
	explicit AutoBson(bson_t *b):m_b(b){}
	~AutoBson()
	{
		SAFE_DESTROY_BSON(m_b);
	}
private:
	bson_t *m_b = nullptr;
};

}

#endif /* CODE_LBSSERVER_SRC_BSONUTIL_H_ */
