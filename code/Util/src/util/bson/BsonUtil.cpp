/*
 * BsonUtil.cpp
 *
 *  Created on: 2020年1月7日
 *      Author: Tommy
 */
#include "BsonUtil.h"

namespace util
{

/*
bson_oid_t oid;
bson_t *cond = BCON_NEW ("_id", BCON_OID (&oid));//条件为id "_id" : ObjectId("55ef549236fe322f9490e17b")
return cond;
 * */
bson_t *GetBsonDoc(const char* field/*=_id*/,bson_oid_t &oid)
{
    bson_t *cond = BCON_NEW (field, BCON_OID (&oid));//条件为id "_id" : ObjectId("55ef549236fe322f9490e17b")
    return cond;
}

bson_t *GetBsonDoc(const char* field,int value)
{
    return  BCON_NEW(field, BCON_INT32(value));
}

bson_t *GetBsonDoc(const char* field,const char* value)
{
    return  BCON_NEW(field, BCON_UTF8(value));
}
bson_t *GetBsonDoc(const util::CJsonObject &jObj,bson_error_t &error)
{
    const std::string jObjStr = jObj.ToString();
    bson_t * doc = bson_new_from_json((const unsigned char*) jObjStr.c_str(),(uint32) jObjStr.length(), &error);
    return doc;
}
bson_t *GetBsonDoc(const std::string &jJsonObjStr,bson_error_t &error)
{
    bson_t * doc = bson_new_from_json((const unsigned char*) jJsonObjStr.c_str(),(uint32) jJsonObjStr.length(), &error);
    return doc;
}

bson_t *GetBsonDocInt64(const char* field,long long value)
{
    return  BCON_NEW(field, BCON_INT64(value));
}
bson_t *GetBsonDocDouble(const char* field,double value)
{
    return  BCON_NEW(field, BCON_DOUBLE(value));
}
bson_t *GetBsonDocBool(const char* field,bool value)
{
    return  BCON_NEW(field, BCON_BOOL(value));
}

CBsonObject& CBsonObject::operator=(const CBsonObject& oBsonObject)
{
    Parse(oBsonObject.GetBson());
    return(*this);
}

bool CBsonObject::operator==(const CBsonObject& oBsonObject) const
{
    return(this->ToString() == oBsonObject.ToString());
}

bool CBsonObject::Parse(const std::string& strJson)
{
	SAFE_DESTROY_BSON (m_bson);
	m_bson = bson_new_from_json((const unsigned char*) strJson.c_str(),(uint32_t) strJson.size(), &m_error);
	if (m_bson == nullptr)
	{
		m_bson = bson_new ();
		return false;
	}
	return true;
}

bool CBsonObject::Parse(const bson_t* bson)
{
	if (bson == nullptr)
	{
		return false;
	}
	SAFE_DESTROY_BSON (m_bson);
	m_bson = bson_copy(bson);
	return true;
}

std::string CBsonObject::ToString() const
{
	if (m_bson)
	{
		char *bStr = bson_as_json_with_opts(m_bson, NULL, NULL);
		std::string str(bStr);
		bson_free(bStr);
		return str;
	}
	return "";
}

bool CBsonObject::IsEmpty() const
{
	return bson_empty(m_bson);
}

void CBsonObject::Clear()
{
	SAFE_DESTROY_BSON (m_bson);
	m_bson = bson_new ();
}

void CBsonObject::Add(const std::string& strKey,const CBsonObject& object)
{
	bson_append_document (m_bson, strKey.c_str(),strKey.size(), object.GetBson());
}
/*
BSON_APPEND_INT32 (&b, "a", 1);
BSON_APPEND_UTF8 (&b, "hello", "world");
BSON_APPEND_BOOL (&b, "bool", true);
 * */
void CBsonObject::Add(const std::string& strKey,int value)
{
    BSON_APPEND_INT32 (m_bson, strKey.c_str(), value);
}

void CBsonObject::Add(const std::string& strKey,int64_t value)
{
    BSON_APPEND_INT64 (m_bson, strKey.c_str(), value);
}

void CBsonObject::Add(const std::string& strKey,double value)
{
	BSON_APPEND_DOUBLE (m_bson, strKey.c_str(), value);
}

void CBsonObject::Add(const std::string& strKey,bool value)
{
    BSON_APPEND_BOOL (m_bson, strKey.c_str(), value);
}

void CBsonObject::Add(const std::string& strKey,const char* value)
{
    BSON_APPEND_UTF8 (m_bson, strKey.c_str(), value);
}

void CBsonObject::Add(const std::string& strKey,const std::string& value)
{
    BSON_APPEND_UTF8 (m_bson, strKey.c_str(), value.c_str());
}
/*
bson_t parent;
bson_t child;
char *str;

bson_init (&parent);
bson_append_document_begin (&parent, "$set", 3, &child);
bson_append_int32 (&child, "baz", 3, 1);
bson_append_document_end (&parent, &child);

str = bson_as_json (&parent, NULL);
printf ("%s\n", str);
bson_free (str);

bson_destroy (&parent);
 * */
void CBsonObject::Add(const std::string& strKey/*="$set"*/,const bson_t *value)
{
	bson_append_document (m_bson, strKey.c_str(),strKey.size(), value);
}

//b, key, subtype, val, len
void CBsonObject::Add(const std::string& strKey,const uint8_t* value,uint32_t len)
{
	BSON_APPEND_BINARY(m_bson, strKey.c_str(), BSON_SUBTYPE_BINARY, value,len);
}

void CBsonObject::Add(const std::string& strKey,std::vector<int> &value)
{
	bson_t labels;
	BSON_APPEND_ARRAY_BEGIN (m_bson, strKey.c_str(), &labels);
	for(int i = 0; i < value.size();++i)
	{
		BSON_APPEND_INT32 (&labels, std::to_string(i).c_str(),value[i]);
	}
    bson_append_array_end (m_bson, &labels);
}

void CBsonObject::Add(const std::string& strKey,std::vector<int64_t> &value)
{
	bson_t labels;
	BSON_APPEND_ARRAY_BEGIN (m_bson, strKey.c_str(), &labels);
	for(int i = 0; i < value.size();++i)
	{
		BSON_APPEND_INT64 (&labels, std::to_string(i).c_str(),value[i]);
	}
    bson_append_array_end (m_bson, &labels);
}

void CBsonObject::Add(const std::string& strKey,std::vector<std::string> &value)
{
	bson_t labels;
	BSON_APPEND_ARRAY_BEGIN (m_bson, strKey.c_str(), &labels);
	for(int i = 0; i < value.size();++i)
	{
		BSON_APPEND_UTF8 (&labels, std::to_string(i).c_str(),value[i].c_str());
	}
	bson_append_array_end (m_bson, &labels);
}

void CBsonObject::AddDataTime(const std::string& strKey,int64_t value)
{
	BSON_APPEND_DATE_TIME (m_bson, strKey.c_str(), value);
}

void CBsonObject::AddTimeStamp(const std::string& strKey,uint32_t value,uint32_t increment)
{
	BSON_APPEND_TIMESTAMP (m_bson, strKey.c_str(), value,increment);
}

void CBsonObject::AddBsonValue(const std::string& strKey,const bson_value_t *value)
{
	BSON_APPEND_VALUE (m_bson, strKey.c_str(), value);
}

void CBsonObject::AddCode(const std::string& strKey,const std::string& strCode)
{
	BSON_APPEND_CODE (m_bson, strKey.c_str(), strCode.c_str());
}

void CBsonObject::AddSymbol(const std::string& strKey,const std::string& strSymbol)
{
	BSON_APPEND_SYMBOL (m_bson, strKey.c_str(), strSymbol.c_str());
}


void CBsonObject::AddUndefined(const std::string& strKey)
{
	BSON_APPEND_UNDEFINED (m_bson, strKey.c_str());
}

bool CBsonObject::GetBinary(const std::string& strKey, std::string& strValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	bson_subtype_t subtype;
	uint32_t binary_len;
	const uint8_t *binary = nullptr;
	bson_iter_binary (&iter, &subtype, &binary_len, &binary);
	if (binary_len > 0 && binary != nullptr)
	{
		strValue.assign((const char*)binary,binary_len);
	}
	return true;
}

bool CBsonObject::GetArray(const std::string& strKey, std::string& strValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	const uint8_t *buf = nullptr;
    uint32_t buf_len(0);
	bson_iter_array (&iter, &buf_len, &buf);
	if (buf_len > 0 && buf != nullptr)
	{
		strValue.assign((const char*)buf,buf_len);
	}
	return true;
}

bool CBsonObject::GetKeys(std::vector<std::string> &keys)const
{
	bson_iter_t iter;
	if (!bson_iter_init(&iter, m_bson)) return false;
	const char *key;
	while (bson_iter_next(&iter)) {
	      key = bson_iter_key(&iter);
	      keys.push_back(key);
	}
	return true;
}


bool CBsonObject::Get(const std::string& strKey, std::string& strValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	uint32_t strLen(0);
	const char * str = bson_iter_utf8 (&iter, &strLen);
	if (strLen > 0 && str != nullptr)
	{
		strValue.assign(str,strLen);
	}
	return true;
}



bool CBsonObject::Get(const std::string& strKey, int32_t& iValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	iValue = bson_iter_int32 (&iter);
	return true;
}

bool CBsonObject::Get(const std::string& strKey, uint32_t& uiValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	uiValue = bson_iter_int32 (&iter);
	return true;
}


bool CBsonObject::Get(const std::string& strKey, int64_t& llValue) const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	llValue = bson_iter_int64 (&iter);
	return true;
}

bool CBsonObject::Get(const std::string& strKey, uint64_t& llValue) const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	llValue = bson_iter_int64 (&iter);
	return true;
}

bool CBsonObject::Get(const std::string& strKey, bool& bValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	bValue = bson_iter_bool (&iter);
	return true;
}
bool CBsonObject::Get(const std::string& strKey, float& fValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	fValue = bson_iter_double (&iter);
	return true;
}
bool CBsonObject::Get(const std::string& strKey, double& dValue)const
{
	bson_iter_t iter;
	if  (!bson_iter_init_find_case (&iter, m_bson, strKey.c_str()))
	{
		return false;
	}
	dValue = bson_iter_double (&iter);
	return true;
}



bool CBsonObject::ToJson(util::CJsonObject &jsonObj)
{
	if (m_bson)
	{
		char *bStr = bson_as_json_with_opts(m_bson, NULL, NULL);
		jsonObj.Clear();
		if (!jsonObj.Parse(bStr))
		{
			bson_free(bStr);
			return false;
		}
		bson_free(bStr);
		return true;
	}
	return false;
}


std::string CBsonObject::ToJsonStr()
{
	std::string tmpStr;
	if (m_bson)
	{
		char *bStr = bson_as_json_with_opts(m_bson, NULL, NULL);
		tmpStr.assign(bStr);
		bson_free(bStr);
	}
	return tmpStr;
}


void BsonToJson(const bson_t *bson,util::CJsonObject &jsonObj)
{
	if (bson)
	{
		char *bStr = bson_as_json_with_opts(bson, NULL,NULL);
		jsonObj.Clear();
		jsonObj.Parse(bStr);
		bson_free(bStr);
	}
}


}

