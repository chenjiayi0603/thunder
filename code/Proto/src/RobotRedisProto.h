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

enum E_REDIS_ROBOT_DATA
{
    /*属性型数据 */
    IM_DATA_TEST              	 = 1000,    ///< 测试
    
    IM_DATA_DOMAIN               = 1,    ///< app域名
    IM_DATA_ACCOUNT              = 2,    ///< account数据
    IM_DATA_CREATE_USERID        = 3,    ///< 创建账号，userid生成
    IM_DATA_USER_INFO            = 4,    ///< 用户基础属性数据，对应数据库中db_userinfo.tb_userinfo_xx
    IM_DATA_USER_ONLINE          = 5,    ///< 用户在线状态
    IM_DATA_APP_INFO               = 27,    ///< app应用信息

    /*日志型数据 > 1000 */
};



#endif /* REDISDATA_HPP_ */
