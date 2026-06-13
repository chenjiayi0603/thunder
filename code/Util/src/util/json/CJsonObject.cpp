/*******************************************************************************
 * Project:  Util
 * @file     CJsonObject.cpp
 * @brief    Json管理类 (yyjson backend)
 ******************************************************************************/

#include "CJsonObject.hpp"
#include <utility>

namespace util
{

// ─────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────

// Convert an atomic val to its scalar string representation.
// Returns "" for null/object/array.
std::string CJsonObject::ValToStr(yyjson_mut_val* v)
{
    if (!v) return "";
    if (yyjson_mut_is_str(v))
        return yyjson_mut_get_str(v);
    if (yyjson_mut_is_sint(v)) {
        int64_t i = yyjson_mut_get_sint(v);
        char buf[32];
        if (i >= INT_MIN && i <= INT_MAX)
            snprintf(buf, sizeof(buf), "%d", (int)i);
        else
            snprintf(buf, sizeof(buf), "%lld", (long long)i);
        return buf;
    }
    if (yyjson_mut_is_uint(v)) {
        uint64_t u = yyjson_mut_get_uint(v);
        char buf[32];
        if (u <= UINT_MAX)
            snprintf(buf, sizeof(buf), "%u", (unsigned)u);
        else
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)u);
        return buf;
    }
    if (yyjson_mut_is_real(v)) {
        double d = yyjson_mut_get_real(v);
        char buf[64];
        if (fabs(d) < 1.0e-6 || fabs(d) > 1.0e9)
            snprintf(buf, sizeof(buf), "%e", d);
        else
            snprintf(buf, sizeof(buf), "%f", d);
        return buf;
    }
    if (yyjson_mut_is_true(v))  return "true";
    if (yyjson_mut_is_false(v)) return "false";
    return "";
}

// ─────────────────────────────────────────────────────────────
// Constructors / destructor
// ─────────────────────────────────────────────────────────────

CJsonObject::CJsonObject()
    : m_pOwnedDoc(nullptr), m_pVal(nullptr), m_pDocRef(nullptr)
{
}

CJsonObject::CJsonObject(const std::string& strJson)
    : m_pOwnedDoc(nullptr), m_pVal(nullptr), m_pDocRef(nullptr)
{
    Parse(strJson);
}

CJsonObject::CJsonObject(const CJsonObject* pJsonObject)
    : m_pOwnedDoc(nullptr), m_pVal(nullptr), m_pDocRef(nullptr)
{
    if (pJsonObject)
        Parse(pJsonObject->ToString());
}

CJsonObject::CJsonObject(const CJsonObject& oJsonObject)
    : m_pOwnedDoc(nullptr), m_pVal(nullptr), m_pDocRef(nullptr)
{
    Parse(oJsonObject.ToString());
}

// Borrowed sub-object constructor (private).
CJsonObject::CJsonObject(yyjson_mut_val* pVal, yyjson_mut_doc* pDoc)
    : m_pOwnedDoc(nullptr), m_pVal(pVal), m_pDocRef(pDoc)
{
}

CJsonObject::~CJsonObject()
{
    Clear();
}

// ─────────────────────────────────────────────────────────────
// Core ops
// ─────────────────────────────────────────────────────────────

CJsonObject& CJsonObject::operator=(const CJsonObject& oJsonObject)
{
    if (this != &oJsonObject)
        Parse(oJsonObject.ToString());
    return *this;
}

bool CJsonObject::operator==(const CJsonObject& oJsonObject) const
{
    return ToString() == oJsonObject.ToString();
}

bool CJsonObject::Parse(const std::string& strJson)
{
    Clear();
    yyjson_doc* imut = yyjson_read(strJson.c_str(), strJson.size(), 0);
    if (!imut)
    {
        m_strErrMsg = "parse json string error";
        return false;
    }
    m_pOwnedDoc = yyjson_mut_doc_new(nullptr);
    m_pDocRef   = m_pOwnedDoc;
    m_pVal = yyjson_val_mut_copy(m_pOwnedDoc, yyjson_doc_get_root(imut));
    yyjson_mut_doc_set_root(m_pOwnedDoc, m_pVal);
    yyjson_doc_free(imut);
    return m_pVal != nullptr;
}

void CJsonObject::Clear()
{
    m_mapJsonArrayRef.clear();
    m_mapJsonObjectRef.clear();
    if (m_pOwnedDoc)
    {
        yyjson_mut_doc_free(m_pOwnedDoc);
        m_pOwnedDoc = nullptr;
    }
    m_pVal    = nullptr;
    m_pDocRef = nullptr;
}

bool CJsonObject::IsEmpty() const
{
    return m_pVal == nullptr;
}

bool CJsonObject::IsArray() const
{
    return m_pVal && yyjson_mut_is_arr(m_pVal);
}

bool CJsonObject::IsNull() const
{
    return m_pVal && yyjson_mut_is_null(m_pVal);
}

std::string CJsonObject::ToString() const
{
    if (!m_pVal) return "";
    size_t len;
    char* s = yyjson_mut_val_write(m_pVal, 0, &len);
    if (!s) return "";
    std::string result(s, len);
    free(s);
    return result;
}

std::string CJsonObject::ToFormattedString() const
{
    if (!m_pVal) return "";
    size_t len;
    char* s = yyjson_mut_val_write(m_pVal, YYJSON_WRITE_PRETTY, &len);
    if (!s) return "";
    std::string result(s, len);
    free(s);
    return result;
}

bool CJsonObject::GetAsString(std::string& str) const
{
    if (!m_pVal || !yyjson_mut_is_str(m_pVal)) return false;
    str = yyjson_mut_get_str(m_pVal);
    return true;
}

// ─────────────────────────────────────────────────────────────
// Object helpers
// ─────────────────────────────────────────────────────────────

void CJsonObject::GetKeys(std::vector<std::string>& vecKeys)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return;
    yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(m_pVal);
    yyjson_mut_val* key;
    while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr)
        vecKeys.push_back(yyjson_mut_get_str(key));
}

bool CJsonObject::AddEmptySubObject(const std::string& strKey)
{
    if (!m_pVal)
    {
        m_pOwnedDoc = yyjson_mut_doc_new(nullptr);
        m_pDocRef   = m_pOwnedDoc;
        m_pVal      = yyjson_mut_obj(m_pOwnedDoc);
        yyjson_mut_doc_set_root(m_pOwnedDoc, m_pVal);
    }
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    yyjson_mut_val* sub = yyjson_mut_obj(m_pDocRef);
    return yyjson_mut_obj_add_val(m_pDocRef, m_pVal, strKey.c_str(), sub);
}

bool CJsonObject::AddEmptySubArray(const std::string& strKey)
{
    if (!m_pVal)
    {
        m_pOwnedDoc = yyjson_mut_doc_new(nullptr);
        m_pDocRef   = m_pOwnedDoc;
        m_pVal      = yyjson_mut_obj(m_pOwnedDoc);
        yyjson_mut_doc_set_root(m_pOwnedDoc, m_pVal);
    }
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    yyjson_mut_val* sub = yyjson_mut_arr(m_pDocRef);
    return yyjson_mut_obj_add_val(m_pDocRef, m_pVal, strKey.c_str(), sub);
}

CJsonObject& CJsonObject::operator[](const std::string& strKey)
{
    auto iter = m_mapJsonObjectRef.find(strKey);
    if (iter != m_mapJsonObjectRef.end())
        return *(iter->second);

    yyjson_mut_val* sub = nullptr;
    if (m_pVal && yyjson_mut_is_obj(m_pVal))
        sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());

    if (!sub)
    {
        auto up = std::unique_ptr<CJsonObject>(new CJsonObject());
        CJsonObject* raw = up.get();
        m_mapJsonObjectRef.emplace(strKey, std::move(up));
        return *raw;
    }
    auto up = std::unique_ptr<CJsonObject>(new CJsonObject(sub, m_pDocRef));
    CJsonObject* raw = up.get();
    m_mapJsonObjectRef.emplace(strKey, std::move(up));
    return *raw;
}

CJsonObject& CJsonObject::operator[](unsigned int uiWhich)
{
    auto iter = m_mapJsonArrayRef.find(uiWhich);
    if (iter != m_mapJsonArrayRef.end())
        return *(iter->second);

    yyjson_mut_val* sub = nullptr;
    if (m_pVal && yyjson_mut_is_arr(m_pVal))
        sub = yyjson_mut_arr_get(m_pVal, uiWhich);

    if (!sub)
    {
        auto up = std::unique_ptr<CJsonObject>(new CJsonObject());
        CJsonObject* raw = up.get();
        m_mapJsonArrayRef.emplace(uiWhich, std::move(up));
        return *raw;
    }
    auto up = std::unique_ptr<CJsonObject>(new CJsonObject(sub, m_pDocRef));
    CJsonObject* raw = up.get();
    m_mapJsonArrayRef.emplace(uiWhich, std::move(up));
    return *raw;
}

std::string CJsonObject::operator()(const std::string& strKey)
{
    // Check cache first
    auto cit = m_mapJsonObjectRef.find(strKey);
    if (cit != m_mapJsonObjectRef.end())
        return ValToStr(cit->second->m_pVal);

    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return "";
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (sub)
        m_mapJsonObjectRef.emplace(strKey,
            std::unique_ptr<CJsonObject>(new CJsonObject(sub, m_pDocRef)));
    return ValToStr(sub);
}

std::string CJsonObject::operator()(const std::string& strKey) const
{
    auto cit = m_mapJsonObjectRef.find(strKey);
    if (cit != m_mapJsonObjectRef.end())
        return ValToStr(cit->second->m_pVal);

    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return "";
    return ValToStr(yyjson_mut_obj_get(m_pVal, strKey.c_str()));
}

std::string CJsonObject::operator()(unsigned int uiWhich)
{
    auto cit = m_mapJsonArrayRef.find(uiWhich);
    if (cit != m_mapJsonArrayRef.end())
        return ValToStr(cit->second->m_pVal);

    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return "";
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, uiWhich);
    if (sub)
        m_mapJsonArrayRef.emplace(uiWhich,
            std::unique_ptr<CJsonObject>(new CJsonObject(sub, m_pDocRef)));
    return ValToStr(sub);
}

std::string CJsonObject::operator()(unsigned int uiWhich) const
{
    auto cit = m_mapJsonArrayRef.find(uiWhich);
    if (cit != m_mapJsonArrayRef.end())
        return ValToStr(cit->second->m_pVal);

    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return "";
    return ValToStr(yyjson_mut_arr_get(m_pVal, uiWhich));
}

// ─────────────────────────────────────────────────────────────
// Get from object
// ─────────────────────────────────────────────────────────────

bool CJsonObject::Get(const std::string& strKey, CJsonObject& oJsonObject) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub) return false;
    size_t len;
    char* s = yyjson_mut_val_write(sub, 0, &len);
    if (!s) return false;
    bool ok = oJsonObject.Parse(std::string(s, len));
    free(s);
    return ok;
}

bool CJsonObject::Get(const std::string& strKey, std::string& strValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_str(sub)) return false;
    strValue = yyjson_mut_get_str(sub);
    return true;
}

bool CJsonObject::Get(const std::string& strKey, int32& iValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    iValue = (int32)yyjson_mut_get_sint(sub);
    return true;
}

bool CJsonObject::Get(const std::string& strKey, uint32& uiValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    uiValue = (uint32)yyjson_mut_get_uint(sub);
    return true;
}

bool CJsonObject::Get(const std::string& strKey, int64& llValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    llValue = (int64)yyjson_mut_get_sint(sub);
    return true;
}

bool CJsonObject::Get(const std::string& strKey, uint64& ullValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    ullValue = (uint64)yyjson_mut_get_uint(sub);
    return true;
}

bool CJsonObject::Get(const std::string& strKey, bool& bValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_bool(sub)) return false;
    bValue = yyjson_mut_get_bool(sub);
    return true;
}

bool CJsonObject::Get(const std::string& strKey, float& fValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_real(sub)) return false;
    fValue = (float)yyjson_mut_get_real(sub);
    return true;
}

bool CJsonObject::Get(const std::string& strKey, double& dValue) const
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_obj_get(m_pVal, strKey.c_str());
    if (!sub || !yyjson_mut_is_real(sub)) return false;
    dValue = yyjson_mut_get_real(sub);
    return true;
}

// ─────────────────────────────────────────────────────────────
// Add to object
// ─────────────────────────────────────────────────────────────

static void EnsureObj(yyjson_mut_doc*& doc, yyjson_mut_doc*& docRef, yyjson_mut_val*& val)
{
    if (!val)
    {
        doc    = yyjson_mut_doc_new(nullptr);
        docRef = doc;
        val    = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, val);
    }
}

bool CJsonObject::Add(const std::string& strKey, const CJsonObject& oJsonObject)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    // Deep-copy via JSON roundtrip (same semantics as original cJSON version).
    std::string jsonStr = oJsonObject.ToString();
    if (jsonStr.empty()) return false;
    yyjson_doc* tmp = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    if (!tmp) return false;
    yyjson_mut_val* copied = yyjson_val_mut_copy(m_pDocRef, yyjson_doc_get_root(tmp));
    yyjson_doc_free(tmp);
    if (!copied) return false;
    return yyjson_mut_obj_add_val(m_pDocRef, m_pVal, strKey.c_str(), copied);
}

bool CJsonObject::Add(const std::string& strKey, const std::string& strValue)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_strcpy(m_pDocRef, m_pVal, strKey.c_str(), strValue.c_str());
}

bool CJsonObject::Add(const std::string& strKey, int32 iValue)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_int(m_pDocRef, m_pVal, strKey.c_str(), (int64_t)iValue);
}

bool CJsonObject::Add(const std::string& strKey, uint32 uiValue)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_uint(m_pDocRef, m_pVal, strKey.c_str(), (uint64_t)uiValue);
}

bool CJsonObject::Add(const std::string& strKey, int64 llValue)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_int(m_pDocRef, m_pVal, strKey.c_str(), (int64_t)llValue);
}

bool CJsonObject::Add(const std::string& strKey, uint64 ullValue)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_uint(m_pDocRef, m_pVal, strKey.c_str(), (uint64_t)ullValue);
}

bool CJsonObject::Add(const std::string& strKey, bool bValue, bool /*bValueAgain*/)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_bool(m_pDocRef, m_pVal, strKey.c_str(), bValue);
}

bool CJsonObject::Add(const std::string& strKey, float fValue)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_real(m_pDocRef, m_pVal, strKey.c_str(), (double)fValue);
}

bool CJsonObject::Add(const std::string& strKey, double dValue)
{
    EnsureObj(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "not a json object! json array?";
        return false;
    }
    return yyjson_mut_obj_add_real(m_pDocRef, m_pVal, strKey.c_str(), dValue);
}

// ─────────────────────────────────────────────────────────────
// Delete / Replace (object)
// ─────────────────────────────────────────────────────────────

bool CJsonObject::Delete(const std::string& strKey)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    yyjson_mut_obj_remove_str(m_pVal, strKey.c_str());
    return true;  // match original: always true (idempotent delete)
}

// Helper: remove key, add new val. Returns false if key didn't exist.
static bool ObjReplace(yyjson_mut_doc* doc, yyjson_mut_val* obj,
                       const char* key, yyjson_mut_val* newVal)
{
    if (!yyjson_mut_obj_remove_str(obj, key)) return false;
    return yyjson_mut_obj_add_val(doc, obj, key, newVal);
}

bool CJsonObject::Replace(const std::string& strKey, const CJsonObject& oJsonObject)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    std::string jsonStr = oJsonObject.ToString();
    if (jsonStr.empty()) return false;
    yyjson_doc* tmp = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    if (!tmp) return false;
    yyjson_mut_val* copied = yyjson_val_mut_copy(m_pDocRef, yyjson_doc_get_root(tmp));
    yyjson_doc_free(tmp);
    if (!copied) return false;
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(), copied);
}

bool CJsonObject::Replace(const std::string& strKey, const std::string& strValue)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    yyjson_mut_val* newVal = yyjson_mut_strcpy(m_pDocRef, strValue.c_str());
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(), newVal);
}

bool CJsonObject::Replace(const std::string& strKey, int32 iValue)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(),
                      yyjson_mut_sint(m_pDocRef, (int64_t)iValue));
}

bool CJsonObject::Replace(const std::string& strKey, uint32 uiValue)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(),
                      yyjson_mut_uint(m_pDocRef, (uint64_t)uiValue));
}

bool CJsonObject::Replace(const std::string& strKey, int64 llValue)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(),
                      yyjson_mut_sint(m_pDocRef, (int64_t)llValue));
}

bool CJsonObject::Replace(const std::string& strKey, uint64 ullValue)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(),
                      yyjson_mut_uint(m_pDocRef, (uint64_t)ullValue));
}

bool CJsonObject::Replace(const std::string& strKey, bool bValue, bool /*bValueAgain*/)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(),
                      yyjson_mut_bool(m_pDocRef, bValue));
}

bool CJsonObject::Replace(const std::string& strKey, float fValue)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(),
                      yyjson_mut_real(m_pDocRef, (double)fValue));
}

bool CJsonObject::Replace(const std::string& strKey, double dValue)
{
    if (!m_pVal || !yyjson_mut_is_obj(m_pVal))
    {
        m_strErrMsg = "json data is null or not an object!";
        return false;
    }
    m_mapJsonObjectRef.erase(strKey);
    return ObjReplace(m_pDocRef, m_pVal, strKey.c_str(),
                      yyjson_mut_real(m_pDocRef, dValue));
}

// ─────────────────────────────────────────────────────────────
// Array — size
// ─────────────────────────────────────────────────────────────

int CJsonObject::GetArraySize()
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return 0;
    return (int)yyjson_mut_arr_size(m_pVal);
}

// ─────────────────────────────────────────────────────────────
// Get from array
// ─────────────────────────────────────────────────────────────

bool CJsonObject::Get(int iWhich, CJsonObject& oJsonObject) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub) return false;
    size_t len;
    char* s = yyjson_mut_val_write(sub, 0, &len);
    if (!s) return false;
    bool ok = oJsonObject.Parse(std::string(s, len));
    free(s);
    return ok;
}

bool CJsonObject::Get(int iWhich, std::string& strValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_str(sub)) return false;
    strValue = yyjson_mut_get_str(sub);
    return true;
}

bool CJsonObject::Get(int iWhich, int32& iValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    iValue = (int32)yyjson_mut_get_sint(sub);
    return true;
}

bool CJsonObject::Get(int iWhich, uint32& uiValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    uiValue = (uint32)yyjson_mut_get_uint(sub);
    return true;
}

bool CJsonObject::Get(int iWhich, int64& llValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    llValue = (int64)yyjson_mut_get_sint(sub);
    return true;
}

bool CJsonObject::Get(int iWhich, uint64& ullValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_int(sub)) return false;
    ullValue = (uint64)yyjson_mut_get_uint(sub);
    return true;
}

bool CJsonObject::Get(int iWhich, bool& bValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_bool(sub)) return false;
    bValue = yyjson_mut_get_bool(sub);
    return true;
}

bool CJsonObject::Get(int iWhich, float& fValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_real(sub)) return false;
    fValue = (float)yyjson_mut_get_real(sub);
    return true;
}

bool CJsonObject::Get(int iWhich, double& dValue) const
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal)) return false;
    yyjson_mut_val* sub = yyjson_mut_arr_get(m_pVal, (size_t)iWhich);
    if (!sub || !yyjson_mut_is_real(sub)) return false;
    dValue = yyjson_mut_get_real(sub);
    return true;
}

// ─────────────────────────────────────────────────────────────
// Add to array
// ─────────────────────────────────────────────────────────────

static void EnsureArr(yyjson_mut_doc*& doc, yyjson_mut_doc*& docRef, yyjson_mut_val*& val)
{
    if (!val)
    {
        doc    = yyjson_mut_doc_new(nullptr);
        docRef = doc;
        val    = yyjson_mut_arr(doc);
        yyjson_mut_doc_set_root(doc, val);
    }
}

bool CJsonObject::Add(const CJsonObject& oJsonObject)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    std::string jsonStr = oJsonObject.ToString();
    if (jsonStr.empty()) return false;
    yyjson_doc* tmp = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    if (!tmp) return false;
    yyjson_mut_val* copied = yyjson_val_mut_copy(m_pDocRef, yyjson_doc_get_root(tmp));
    yyjson_doc_free(tmp);
    if (!copied) return false;
    size_t before = yyjson_mut_arr_size(m_pVal);
    bool ok = yyjson_mut_arr_append(m_pVal, copied);
    if (ok)
    {
        // Invalidate cached entries at or beyond the new last index.
        unsigned int newIdx = (unsigned int)before;
        for (auto it = m_mapJsonArrayRef.begin(); it != m_mapJsonArrayRef.end(); )
        {
            if (it->first >= newIdx) it = m_mapJsonArrayRef.erase(it);
            else ++it;
        }
    }
    return ok;
}

bool CJsonObject::Add(const std::string& strValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_strcpy(m_pDocRef, m_pVal, strValue.c_str());
}

bool CJsonObject::Add(int32 iValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_int(m_pDocRef, m_pVal, (int64_t)iValue);
}

bool CJsonObject::Add(uint32 uiValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_val(m_pVal, yyjson_mut_uint(m_pDocRef, (uint64_t)uiValue));
}

bool CJsonObject::Add(int64 llValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_int(m_pDocRef, m_pVal, (int64_t)llValue);
}

bool CJsonObject::Add(uint64 ullValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_val(m_pVal, yyjson_mut_uint(m_pDocRef, (uint64_t)ullValue));
}

bool CJsonObject::Add(int /*iAnywhere*/, bool bValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_bool(m_pDocRef, m_pVal, bValue);
}

bool CJsonObject::Add(float fValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_real(m_pDocRef, m_pVal, (double)fValue);
}

bool CJsonObject::Add(double dValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    return yyjson_mut_arr_add_real(m_pDocRef, m_pVal, dValue);
}

// ─────────────────────────────────────────────────────────────
// AddAsFirst (array prepend)
// ─────────────────────────────────────────────────────────────

bool CJsonObject::AddAsFirst(const CJsonObject& oJsonObject)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    std::string jsonStr = oJsonObject.ToString();
    if (jsonStr.empty()) return false;
    yyjson_doc* tmp = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    if (!tmp) return false;
    yyjson_mut_val* copied = yyjson_val_mut_copy(m_pDocRef, yyjson_doc_get_root(tmp));
    yyjson_doc_free(tmp);
    if (!copied) return false;
    bool ok = yyjson_mut_arr_prepend(m_pVal, copied);
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(const std::string& strValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_strcpy(m_pDocRef, strValue.c_str()));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(int32 iValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_sint(m_pDocRef, (int64_t)iValue));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(uint32 uiValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_uint(m_pDocRef, (uint64_t)uiValue));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(int64 llValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_sint(m_pDocRef, (int64_t)llValue));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(uint64 ullValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_uint(m_pDocRef, (uint64_t)ullValue));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(int /*iAnywhere*/, bool bValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_bool(m_pDocRef, bValue));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(float fValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_real(m_pDocRef, (double)fValue));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

bool CJsonObject::AddAsFirst(double dValue)
{
    EnsureArr(m_pOwnedDoc, m_pDocRef, m_pVal);
    if (!yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "not a json array! json object?";
        return false;
    }
    bool ok = yyjson_mut_arr_prepend(m_pVal, yyjson_mut_real(m_pDocRef, dValue));
    if (ok) m_mapJsonArrayRef.clear();
    return ok;
}

// ─────────────────────────────────────────────────────────────
// Delete / Replace (array)
// ─────────────────────────────────────────────────────────────

bool CJsonObject::Delete(int iWhich)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    yyjson_mut_val* removed = yyjson_mut_arr_remove(m_pVal, (size_t)iWhich);
    if (!removed) return false;
    // Invalidate cached entries at or beyond removed index.
    for (auto it = m_mapJsonArrayRef.begin(); it != m_mapJsonArrayRef.end(); )
    {
        if (it->first >= (unsigned int)iWhich)
            it = m_mapJsonArrayRef.erase(it);
        else
            ++it;
    }
    return true;
}

bool CJsonObject::Replace(int iWhich, const CJsonObject& oJsonObject)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    std::string jsonStr = oJsonObject.ToString();
    if (jsonStr.empty()) return false;
    yyjson_doc* tmp = yyjson_read(jsonStr.c_str(), jsonStr.size(), 0);
    if (!tmp) return false;
    yyjson_mut_val* copied = yyjson_val_mut_copy(m_pDocRef, yyjson_doc_get_root(tmp));
    yyjson_doc_free(tmp);
    if (!copied) return false;
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich, copied) != nullptr;
}

bool CJsonObject::Replace(int iWhich, const std::string& strValue)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_strcpy(m_pDocRef, strValue.c_str())) != nullptr;
}

bool CJsonObject::Replace(int iWhich, int32 iValue)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_sint(m_pDocRef, (int64_t)iValue)) != nullptr;
}

bool CJsonObject::Replace(int iWhich, uint32 uiValue)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_uint(m_pDocRef, (uint64_t)uiValue)) != nullptr;
}

bool CJsonObject::Replace(int iWhich, int64 llValue)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_sint(m_pDocRef, (int64_t)llValue)) != nullptr;
}

bool CJsonObject::Replace(int iWhich, uint64 ullValue)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_uint(m_pDocRef, (uint64_t)ullValue)) != nullptr;
}

bool CJsonObject::Replace(int iWhich, bool bValue, bool /*bValueAgain*/)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_bool(m_pDocRef, bValue)) != nullptr;
}

bool CJsonObject::Replace(int iWhich, float fValue)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_real(m_pDocRef, (double)fValue)) != nullptr;
}

bool CJsonObject::Replace(int iWhich, double dValue)
{
    if (!m_pVal || !yyjson_mut_is_arr(m_pVal))
    {
        m_strErrMsg = "json data is null or not an array!";
        return false;
    }
    m_mapJsonArrayRef.erase((unsigned int)iWhich);
    return yyjson_mut_arr_replace(m_pVal, (size_t)iWhich,
        yyjson_mut_real(m_pDocRef, dValue)) != nullptr;
}

}
