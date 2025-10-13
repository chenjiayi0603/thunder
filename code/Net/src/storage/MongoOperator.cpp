/*******************************************************************************
 * Project:  RedisAgent
 * @file     DbOperater.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月19日
 * @note
 * Modify history:
 ******************************************************************************/
#include "MongoOperator.hpp"

namespace net
{

MongoOperator::MongoOperator(
                uint64 uiSectionFactor,
                const std::string& strTableName,
                DataMem::MemOperate::MongoOperate::E_QUERY_TYPE eQueryType,
                uint64 uiModFactor)
    : m_uiSectionFactor(uiSectionFactor)
{
    m_pMongoOperate = new DataMem::MemOperate::MongoOperate();
    m_pMongoOperate->set_table_name(strTableName);
    m_pMongoOperate->set_query_type(eQueryType);
    if (uiModFactor > 0)
    {
        m_pMongoOperate->set_mod_factor(uiModFactor);
    }
    else if (uiSectionFactor > 0)
    {
        m_pMongoOperate->set_mod_factor(uiSectionFactor);
    }
}

MongoOperator::~MongoOperator()
{
    if (m_pDbMemRequest != nullptr)
    {
    	SAFE_DELETE(m_pDbMemRequest);
    }
    else
    {
    	SAFE_DELETE(m_pMongoOperate);
    }
}

DataMem::MemOperate* MongoOperator::MakeMemOperate()
{
    if (m_pDbMemRequest == nullptr)
    {
        m_pDbMemRequest = new DataMem::MemOperate();
    }
    else
    {
        return(m_pDbMemRequest);
    }
    if (m_vecPinelineCmds.size() > 0)
	{
		for(auto& cmd:m_vecPinelineCmds)
		{
			m_pMongoOperate->add_raw_cmds(std::move(cmd));
		}
		m_vecPinelineCmds.clear();
	}
    m_pDbMemRequest->set_section_factor(m_uiSectionFactor);
    m_pDbMemRequest->set_allocated_mongo_operate(m_pMongoOperate);
    return(m_pDbMemRequest);
}


//////////////////////////////////////////////////////////////////////////////
//	///< used for mongodb..
//	message MongoOperate
//	{
//		optional E_QUERY_TYPE query_type  = 1;              ///< 查询类型
//		optional string table_name        = 2;              ///< 表名
//		optional string cond              = 4;              ///< 条件json
//		optional string val              = 5;               ///< 值json
//		optional string fields              = 6;            ///<fields json,为空则全部
//		optional string sort              = 7;              ///<sort json,只是支持UPSERT
//
//		optional uint32 limit             = 8;              ///< 指定返回的行数的最大值  (limit 200)
//		optional uint32 limit_from        = 9;              ///< 指定返回的第一行的偏移量 (limit 100, 200)
//		optional uint32 batch_size        = 10;              ///< 批操作每次处理的数据的大小，0则不限制
//		optional uint64 mod_factor        = 11;             ///< 分表取模因子，当这个字段没有时使用section_factor
//
//		enum E_QUERY_TYPE                                   ///< 查询类型
//		{
//		    E_QUERY_TYPE_UNKNONW          = 0;              ///< 未知
//			SELECT                        = 1;              ///< select查询
//			INSERT                        = 2;              ///< insert插入
//			UPDATE                        = 3;              ///< update更新
//			UPSERT                        = 4;              ///< upsert覆盖插入
//			DELETE                        = 5;              ///< delete删除
//			CUSTOM 						  = 6;				///< 自定义(特殊命令)
//		}
//	}

void MongoOperator::AddCond(const std::string& strCond)
{
	m_pMongoOperate->set_cond(strCond);
}

void MongoOperator::AddCond(const bson_t *bson)
{
	char *str = bson_as_json_with_opts(bson, NULL, NULL);
	m_pMongoOperate->set_cond(str);
	bson_free(str);
}

void MongoOperator::AddVal(const std::string& strValue)
{
	m_pMongoOperate->set_val(strValue);
}


void MongoOperator::AddVal(const bson_t *bson)
{
	char *str = bson_as_json_with_opts(bson, NULL, NULL);
	m_pMongoOperate->set_val(str);
	bson_free(str);
}

void MongoOperator::AddFields(const std::string& strFields)
{
	m_pMongoOperate->set_fields(strFields);
}

void MongoOperator::AddFields(const bson_t *bson)
{
	char *str = bson_as_json_with_opts(bson, NULL, NULL);
	m_pMongoOperate->set_fields(str);
	bson_free(str);
}

void MongoOperator::AddSort(const std::string& strSort)
{
	m_pMongoOperate->set_sort(strSort);
}

void MongoOperator::AddSort(const bson_t *bson)
{
	char *str = bson_as_json_with_opts(bson, NULL, NULL);
	m_pMongoOperate->set_sort(str);
	bson_free(str);
}

void MongoOperator::AddLimit(unsigned int uiLimit, unsigned int uiLimitFrom,unsigned int uiBatchSize)
{
    m_pMongoOperate->set_limit(uiLimit);
    if (uiLimitFrom > 0)
    {
        m_pMongoOperate->set_limit_from(uiLimitFrom);
    }
    if (uiBatchSize > 0)
    {
    	m_pMongoOperate->set_batch_size(uiBatchSize);
    }
}

} /* namespace net */
