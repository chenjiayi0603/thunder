/*******************************************************************************
* Project:  proto
* @file     RedisData.hpp
* @brief    Redis数据结构定义
* @author   cjy
* @date:    2015年11月11日
* @note
* Modify history:
******************************************************************************/
#ifndef REDISDATA_HPP_
#define REDISDATA_HPP_

enum E_REDIS_TYPE
{
    REDIS_T_HASH                = 1,    ///< redis hash
    REDIS_T_SET                 = 2,    ///< redis set
    REDIS_T_KEYS                = 3,    ///< redis keys
    REDIS_T_STRING              = 4,    ///< redis string
    REDIS_T_LIST                = 5,    ///< redis list
    REDIS_T_SORT_SET            = 6,    ///< redis sort set
};

enum E_REDIS_ANALYSIS_DATA
{
    /*属性型数据 */
    /*日志型数据*/
};



#endif /* REDISDATA_HPP_ */
