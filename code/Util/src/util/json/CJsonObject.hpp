/*******************************************************************************
 * Project:  Util
 * @file     CJsonObject.hpp
 * @brief    Json管理类 (yyjson backend)
 ******************************************************************************/
#ifndef CJSONOBJECT_HPP_
#define CJSONOBJECT_HPP_
#include <stdio.h>
#include <stddef.h>
#include <malloc.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <float.h>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <unordered_map>

// Global-scope integer typedefs (previously provided by cJSON.h, kept for ABI compatibility).
#ifndef THUNDER_INT_TYPEDEFS
#define THUNDER_INT_TYPEDEFS
typedef int                int32;
typedef unsigned int       uint32;
typedef long long int      int64;
typedef unsigned long long uint64;
#endif

#include "yyjson.h"


namespace util
{

/**
 * @brief Json管理类 (backed by yyjson)
 */
class CJsonObject
{
public:     // method of ordinary json object or json array
    CJsonObject();
    CJsonObject(const std::string& strJson);
    CJsonObject(const CJsonObject* pJsonObject);
    CJsonObject(const CJsonObject& oJsonObject);
    virtual ~CJsonObject();

    CJsonObject& operator=(const CJsonObject& oJsonObject);
    bool operator==(const CJsonObject& oJsonObject) const;
    bool Parse(const std::string& strJson);
    void Clear();
    bool IsEmpty() const;
    bool IsArray() const;
    bool IsNull() const;
    std::string ToString() const;
    std::string ToFormattedString() const;
    const std::string& GetErrMsg() const
    {
        return(m_strErrMsg);
    }

public:     // method of ordinary json object
    void GetKeys(std::vector<std::string> &vecKeys);
    bool AddEmptySubObject(const std::string& strKey);
    bool AddEmptySubArray(const std::string& strKey);
    CJsonObject& operator[](const std::string& strKey);
    std::string operator()(const std::string& strKey);
    std::string operator()(const std::string& strKey)const;
    bool Get(const std::string& strKey, CJsonObject& oJsonObject) const;
    bool Get(const std::string& strKey, std::string& strValue) const;
    bool Get(const std::string& strKey, int32& iValue) const;
    bool Get(const std::string& strKey, uint32& uiValue) const;
    bool Get(const std::string& strKey, int64& llValue) const;
    bool Get(const std::string& strKey, uint64& ullValue) const;
    bool Get(const std::string& strKey, bool& bValue) const;
    bool Get(const std::string& strKey, float& fValue) const;
    bool Get(const std::string& strKey, double& dValue) const;
    bool Add(const std::string& strKey, const CJsonObject& oJsonObject);
    bool Add(const std::string& strKey, const std::string& strValue);
    bool Add(const std::string& strKey, int32 iValue);
    bool Add(const std::string& strKey, uint32 uiValue);
    bool Add(const std::string& strKey, int64 llValue);
    bool Add(const std::string& strKey, uint64 ullValue);
    bool Add(const std::string& strKey, bool bValue, bool bValueAgain);
    bool Add(const std::string& strKey, float fValue);
    bool Add(const std::string& strKey, double dValue);
    bool Delete(const std::string& strKey);
    bool Replace(const std::string& strKey, const CJsonObject& oJsonObject);
    bool Replace(const std::string& strKey, const std::string& strValue);
    bool Replace(const std::string& strKey, int32 iValue);
    bool Replace(const std::string& strKey, uint32 uiValue);
    bool Replace(const std::string& strKey, int64 llValue);
    bool Replace(const std::string& strKey, uint64 ullValue);
    bool Replace(const std::string& strKey, bool bValue, bool bValueAgain);
    bool Replace(const std::string& strKey, float fValue);
    bool Replace(const std::string& strKey, double dValue);

    // Returns true and sets str if the root node is a plain string (not an object/array).
    bool GetAsString(std::string& str) const;

public:     // method of json array
    int GetArraySize();
    CJsonObject& operator[](unsigned int uiWhich);
    std::string operator()(unsigned int uiWhich);
    std::string operator()(unsigned int uiWhich)const;
    bool Get(int iWhich, CJsonObject& oJsonObject) const;
    bool Get(int iWhich, std::string& strValue) const;
    bool Get(int iWhich, int32& iValue) const;
    bool Get(int iWhich, uint32& uiValue) const;
    bool Get(int iWhich, int64& llValue) const;
    bool Get(int iWhich, uint64& ullValue) const;
    bool Get(int iWhich, bool& bValue) const;
    bool Get(int iWhich, float& fValue) const;
    bool Get(int iWhich, double& dValue) const;
    bool Add(const CJsonObject& oJsonObject);
    bool Add(const std::string& strValue);
    bool Add(int32 iValue);
    bool Add(uint32 uiValue);
    bool Add(int64 llValue);
    bool Add(uint64 ullValue);
    bool Add(int iAnywhere, bool bValue);
    bool Add(float fValue);
    bool Add(double dValue);
    bool AddAsFirst(const CJsonObject& oJsonObject);
    bool AddAsFirst(const std::string& strValue);
    bool AddAsFirst(int32 iValue);
    bool AddAsFirst(uint32 uiValue);
    bool AddAsFirst(int64 llValue);
    bool AddAsFirst(uint64 ullValue);
    bool AddAsFirst(int iAnywhere, bool bValue);
    bool AddAsFirst(float fValue);
    bool AddAsFirst(double dValue);
    bool Delete(int iWhich);
    bool Replace(int iWhich, const CJsonObject& oJsonObject);
    bool Replace(int iWhich, const std::string& strValue);
    bool Replace(int iWhich, int32 iValue);
    bool Replace(int iWhich, uint32 uiValue);
    bool Replace(int iWhich, int64 llValue);
    bool Replace(int iWhich, uint64 ullValue);
    bool Replace(int iWhich, bool bValue, bool bValueAgain);
    bool Replace(int iWhich, float fValue);
    bool Replace(int iWhich, double dValue);

private:
    // Borrowed sub-object constructor: val and doc are owned by parent.
    CJsonObject(yyjson_mut_val* pVal, yyjson_mut_doc* pDoc);
    // Convert atomic val to its string representation (for operator()).
    static std::string ValToStr(yyjson_mut_val* v);

private:
    yyjson_mut_doc* m_pOwnedDoc;    // null in borrowed sub-object mode
    yyjson_mut_val* m_pVal;         // root val (in owned or parent's doc)
    yyjson_mut_doc* m_pDocRef;      // allocator doc (= m_pOwnedDoc or parent's)
    std::string m_strErrMsg;
    std::unordered_map<unsigned int, std::unique_ptr<CJsonObject>> m_mapJsonArrayRef;
    std::unordered_map<std::string, std::unique_ptr<CJsonObject>> m_mapJsonObjectRef;
};

}

#endif /* CJSONOBJECT_HPP_ */
