/*******************************************************************************
 * Project:  RedisAgent
 * @file     MongoOperator.hpp
 * @brief    数据库操作协议打包
 * @author   Tommy
 * @date:    2019年11月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef CLIENT_MONGODBOPERATOR_HPP_
#define CLIENT_MONGODBOPERATOR_HPP_
#include <mongoc/src/libbson/src/bson/bson.h>
#include "StorageOperator.hpp"

namespace net
{

/**
 * @brief 数据库（mongodb）请求协议生成器
 * @note 存储请求协议生成，用于同时单独请求数据库的场景。
 */
class MongoOperator : public StorageOperator
{
public:
    /**
     * @brief 创建数据库表操作类
     * @param uiSectionFactor   hash分段因子
     * @param strTableName      表名
     * @param eQueryType        查询类型（SELECT，INSERT，REPLACE等）
     * @param uiModFactor       分表取模因子，当该参数为0时使用uiSectionFactor
     */
	MongoOperator(uint64 uiSectionFactor,
                    const std::string& strTableName,
                    DataMem::MemOperate::MongoOperate::E_QUERY_TYPE eQueryType,
                    uint64 uiModFactor = 0);
    virtual ~MongoOperator();

    virtual DataMem::MemOperate* MakeMemOperate();
    /**
	 * @brief 添加条件
	 * @param strCond json 字段值
	 */
    void AddCond(const std::string& strCond);
    void AddCond(const bson_t *bson);
    /**
	 * @brief 添加条件
	 * @param strVal json 字段值
	 */
    void AddVal(const std::string& strVal);
    void AddVal(const bson_t *bson);
    /**
     * @brief 添加筛选字段
     * @param strFields json 字符串
     */
    void AddFields(const std::string& strFields);
    void AddFields(const bson_t *bson);
    /**
	 * @brief 添加排序字段
	 * @param strSort json 字符串
	 */
    void AddSort(const std::string& strSort);
    void AddSort(const bson_t *bson);
    /**
     * @brief 添加limit记录数限制
     * @param uiLimit 记录条数
     * @param uiLimitFrom 从哪条记录来时限制
     * @param uiBatchSize 批操作每次处理的数据的大小，0则不限制
     * @note 添加记录数限制，AddLimit(100)时， limit 10; AddLimit(100, 200)时，limit 100,200。
     */
    virtual void AddLimit(unsigned int uiLimit, unsigned int uiLimitFrom = 0,unsigned int uiBatchSize = 0);
    virtual bool AddPinelineCmd(const std::string& strCmd)
	{
		if (strCmd.size() > 0)
		{
			m_vecPinelineCmds.push_back(strCmd);
		}
		return true;
	}

	virtual bool AddPinelineCmd(const char *fmt, ...)
	{
		char buf[1024];//目前最长指令设1024,超过长度的调用AddPinelineCmd(const std::string& strCmd)
		va_list args;
		va_start(args, fmt);
		int len = vsnprintf(buf,sizeof(buf),fmt, args);//搜索字符串fmt中需要特定模式的字符(比如%s),args为后面参数的首地址
		va_end(args);
		if (len > 0)
		{
			m_vecPinelineCmds.push_back(std::string(buf,len));
		}
		return true;
	}
	std::vector<std::string> m_vecPinelineCmds;
protected:
    DataMem::MemOperate::MongoOperate* GetDbOperate()
    {
        return(m_pMongoOperate);
    }

    void SetDbOperateNull()
    {
        m_pMongoOperate = nullptr;
    }

private:
    DataMem::MemOperate* m_pDbMemRequest = nullptr;
    DataMem::MemOperate::MongoOperate* m_pMongoOperate = nullptr;
    uint64 m_uiSectionFactor;
};

} /* namespace net */

#endif /* CLIENT_DBOPERATOR_HPP_ */
