/*******************************************************************************
* Project:  proto
* @file     RobotCw.h
* @brief    Robot业务命令字定义
* @author   cjy
* @date:    2015年10月12日
* @note
* Modify history:
******************************************************************************/
#ifndef SRC_ANALYSISCW_H_
#define SRC_ANALYSISCW_H_

namespace analysis
{

/**
 * @brief Robot业务命令字定义
 * @note Robot业务命令字成对出现，从1001开始编号，并且遵从奇数表示请求命令字，
 * 偶数表示应答命令字，应答命令字 = 请求命令字 + 1
 */
enum E_ANALYSIS_CW
{
	CMD_UNDEFINE = 0,			 ///< 未定义
	CMD_REQ_SYS_ERROR = 999,	 ///< 系统错误请求（无意义，不会被使用）
	CMD_RSQ_SYS_ERROR = 1000,	///< 系统错误响应
	CMD_REQ_GET_GMIDINFO = 2001, ///< 补全gmid,user_id,渠道引流信息等
	CMD_RSQ_GET_GMIDINFO = 2002, ///< 响应

};

}   // end of namespace analysis

#endif /* SRC_IMCW_H_ */
