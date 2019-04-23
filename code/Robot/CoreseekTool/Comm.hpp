/*******************************************************************************
 * Project:  RobotServer
 * @file    Comm.hpp
 * @brief    
 * @author   cjy
 * @date:    2017年1月20日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CORESEEK_COMM_HPP_
#define SRC_CORESEEK_COMM_HPP_
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory.h>
#include "log4cplus/logger.h"
#include "log4cplus/fileappender.h"
#include "log4cplus/loggingmacros.h"
#include "util/json/CJsonObject.hpp"
#include "util/CodeConvert.h"
#include "FileUtil.h"

#define LOG4_FATAL(args...) LOG4CPLUS_FATAL_FMT(GetLogger(), ##args)
#define LOG4_ERROR(args...) LOG4CPLUS_ERROR_FMT(GetLogger(), ##args)
#define LOG4_WARN(args...) LOG4CPLUS_WARN_FMT(GetLogger(), ##args)
#define LOG4_INFO(args...) LOG4CPLUS_INFO_FMT(GetLogger(), ##args)
#define LOG4_DEBUG(args...) LOG4CPLUS_DEBUG_FMT(GetLogger(), ##args)
#define LOG4_TRACE(args...) LOG4CPLUS_TRACE_FMT(GetLogger(), ##args)

namespace robot
{
typedef char int8;
typedef unsigned char uint8;
typedef short int16;
typedef unsigned short uint16;
typedef int int32;
typedef unsigned int uint32;
typedef long long int int64;
typedef unsigned long long int uint64;

#define MAKEFOURCC(ch0, ch1, ch2, ch3) ((uint32)(uint8)(ch0) | ((uint32)(uint8)(ch1) << 8) | ((uint32)(uint8)(ch2) << 16) | ((uint32)(uint8)(ch3) << 24 ))

#pragma pack(1)

//自定义排序函数
inline bool SortStringByTime(const std::string &v1, const std::string &v2) //注意：本函数的参数的类型一定要与vector中元素的类型一致
{
    //字符串格式为如 LogData201511241139.fd ，数字为时间
    return v1 < v2; //升序排列
}
//去掉符号
inline void RemoveFlag(std::string &str, char flag = '\"')
{
    std::string::iterator it = std::remove(str.begin(), str.end(), flag);
    str.erase(it, str.end());
}
#pragma pack()

} /* namespace robot */

#endif /* SRC_LOG_COMM_HPP_ */
