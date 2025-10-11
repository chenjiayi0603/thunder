/*******************************************************************************
 * Project:  RedisAgent
 * @file     RedisOperator.hpp
 * @brief    Redis操作协议打包
 * @author   Tommy
 * @date:    2019年11月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef CLIENT_REDISOPERATOR_HPP_
#define CLIENT_REDISOPERATOR_HPP_

#include "StorageOperator.hpp"

namespace net
{

/**
 * @brief Redis请求协议生成器
 * @note 存储请求协议生成，用于同时单独请求Redis的场景。
 */
class RedisOperator : public StorageOperator
{
public:
    /**
     * @brief 存储请求协议生成
     * @note 存储请求协议生成，用于同时单独请求Redis的场景。
     * @param uiSectionFactor   hash分段因子
     * @param strRedisKey       Redis key
     * @param strWriteCmd       Redis写命令（若是读，写命令用于Redis未命中，DataProxy从DB中读取到数据写回Redis时使用）
     * @param strReadCmd        Redis读命令（若是写，读命令为""空串）
     */
    RedisOperator(uint64 uiSectionFactor,
                    const std::string& strRedisKey,
                    const std::string& strWriteCmd,
                    const std::string& strReadCmd = "");
    virtual ~RedisOperator();
    void SetRedisStructure(int structure){m_pRedisOperate->set_redis_structure(structure);}
    virtual DataMem::MemOperate* MakeMemOperate();
    /**
     * @brief 添加字段
     * @note 当写操作Redis与数据库存储的数据字段不相等且Redis Field数量为1或Redis第一个Field的value为空时，
     * 生成的协议包支持，将数据库协议里所有field存储到Redis的Field Value中。详见MemOperator::MakeMemOperate()函数实现。
     * @param strFieldName 字段名（当不是hash结果，是list，set等结构时填写strFieldValue，不填写strFieldName）
     * @param strFieldValue 字段值
     * @return 是否添加成功
     */
    virtual bool AddRedisField(const std::string& strFieldName, const std::string& strFieldValue = "");

    virtual bool AddRedisField(const std::string& strFieldName, int32 iFieldValue);
    virtual bool AddRedisField(const std::string& strFieldName, uint32 uiFieldValue);
    virtual bool AddRedisField(const std::string& strFieldName, int64 llFieldValue);
    virtual bool AddRedisField(const std::string& strFieldName, uint64 ullFieldValue);
    virtual bool AddRedisField(const std::string& strFieldName, float fFieldValue);
    virtual bool AddRedisField(const std::string& strFieldName, double dFieldValue);

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
    uint32 PinelineCmdSize()const {return m_vecPinelineCmds.size();}
    std::vector<std::string> m_vecPinelineCmds;
protected:
    DataMem::MemOperate::RedisOperate* GetRedisOperate()
    {
        return(m_pRedisOperate);
    }

    void SetRedisOperateNull()
    {
        m_pRedisOperate = nullptr;
    }

private:
    DataMem::MemOperate* m_pRedisMemRequest = nullptr;
    DataMem::MemOperate::RedisOperate* m_pRedisOperate = nullptr;
    uint64 m_uiSectionFactor;
};

} /* namespace net */

#endif /* CLIENT_REDISOPERATOR_HPP_ */
