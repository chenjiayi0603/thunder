/*******************************************************************************
 * Project:  LogQueue
 * @file    Comm.hpp
 * @brief    
 * @author   cjy
 * @date:    2015年9月16日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_LOGQUEUE_COMM_HPP_
#define SRC_LOGQUEUE_COMM_HPP_
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
#include "NetError.hpp"
#include "NetDefine.hpp"
#include "behaviour.pb.h"
#include "util/json/CJsonObject.hpp"
#include "util/http/HttpParamCodec.h"
#include "algorithm/Levenshtein.hpp"

namespace analysis
{
typedef net::uint8 uint8;
typedef net::uint16 uint16;
typedef net::uint32 uint32;
typedef net::uint64 uint64;

typedef net::int8 int8;
typedef net::int16 int16;
typedef net::int32 int32;
typedef net::int64 int64;
#pragma pack(1)
/*
数据记录头内容(一共2+2+2+2=8字节)
uint16 nCmdType;//指令类型
uint8 nDataVersion;//数据版本号(服务器)
uint8 nEncript;/// 标识压缩类型、或编码类型(其中高4位表示压缩算法（0表示不压缩），低4位表示加密算法（0表示不加密）)
uint16 nCrc;//冗余校验字段
uint16 nBodySize;//数据记录体大小
 * */
struct LogDataHeader
{
    LogDataHeader()
    {
        memset(this, 0, sizeof(LogDataHeader));
    }
    LogDataHeader(const LogDataHeader& header)
    {
        nLogCmd = header.nLogCmd;
        nDataVersion = header.nDataVersion;
        nEncript = header.nEncript;
        nCrc = header.nCrc;
        nBodySize = header.nBodySize;
    }
    LogDataHeader &operator =(const LogDataHeader &header)
    {
        nLogCmd = header.nLogCmd;
        nDataVersion = header.nDataVersion;
        nEncript = header.nEncript;
        nCrc = header.nCrc;
        nBodySize = header.nBodySize;
        return *this;
    }
    uint16 nLogCmd;//日志指令
    uint8  nDataVersion;//数据版本号
    uint8  nEncript;//标识压缩类型、或编码类型(其中高4位表示压缩算法（0表示不压缩），低4位表示加密算法（0表示不加密）)
    uint16 nCrc;//冗余校验字段
    uint16 nBodySize;//数据记录体大小
    static const uint8 VERSION = 1;
};

#pragma pack()

/*
 "log_list":[
	{
		"log_cmd":1,
		"log_type":"appStartTrace",
		"fields": [
		]
	},
	{
		"log_cmd":2,
		"log_type":"appPageTrace",
		"fields": [
		]
	}
]
 * */
struct LogTable
{
	uint32 nLogCmd;
	std::string strLogType;
	std::vector<std::string> fieldsVec;
	LogTable():nLogCmd(0){}
	LogTable(const LogTable& log)
	{
		nLogCmd = log.nLogCmd;
		strLogType = log.strLogType;
		fieldsVec = log.fieldsVec;
	}
	const LogTable& operator=(const LogTable& log)
	{
		nLogCmd = log.nLogCmd;
		strLogType = log.strLogType;
		fieldsVec = log.fieldsVec;
		return *this;
	}
};

} /* namespace analysis */

#endif /* SRC_LOG_COMM_HPP_ */
