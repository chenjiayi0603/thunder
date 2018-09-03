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
    IM_DATA_USER = 1,       ///用户
    IM_DATA_USERSID = 2,    ///全局用户统计id
    IM_DATA_USERID_MAP = 3, ///用户ID映射
    IM_DATA_DEVICE = 4,       ///设备

    /*属性型数据 gmid相关*/
    IM_DATA_DEVICEID_USERID = 71, ///拉取deviceID对应的userID等信息
    IM_DATA_USERID_DEVICEID = 72, ///拉取userID对应的DEVICEID等信息

    /*日志型数据*/
    IM_DATA_GLOBAL_MSG_BACKUP  = 100,    ///备份消息
    IM_DATA_TRACE_STATISTICS  = 101,    ///事件日志统计
    IM_DATA_TRACE_ATTRIBUTES_STATISTICS  = 102,    ///事件属性日志统计
    IM_DATA_USER_SESSION  = 103,    ///用户会话
};



#endif /* REDISDATA_HPP_ */
