/*
 * CommonUtils.hpp
 *
 *  Created on: 2017年1月17日
 *      author   cjy
 */

#ifndef CODE_LOSS_SRC_UTIL_COMMONUTILS_HPP_
#define CODE_LOSS_SRC_UTIL_COMMONUTILS_HPP_
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <iostream>
#include <string>
#include <sstream>
#include <sys/time.h>
#include <chrono>
#include <vector>

namespace util
{

typedef char int8;
typedef unsigned char uint8;
typedef short int16;
typedef unsigned short uint16;
typedef int int32;
typedef unsigned int uint32;
typedef long long int int64;
typedef unsigned long long int uint64;

#define MAKE_FOUR_CC(ch0, ch1, ch2, ch3) ((uint32)(uint8)(ch0) | ((uint32)(uint8)(ch1) << 8) | ((uint32)(uint8)(ch2) << 16) | ((uint32)(uint8)(ch3) << 24 ))

//默认使用 毫秒级时间戳id
#define GetUniqueId(uiNodeId,ucWorkerIndex)  GetUniqueId_MS(uiNodeId,ucWorkerIndex)

//秒级时间戳id
inline uint64 GetUniqueId_S(unsigned short uiNodeId, unsigned char ucWorkerIndex)
{
	//（可以兼容64位整形）  32bit作为秒数，16bit作为机器的ID（8个bit是节点，8个bit的工作者id），16bit作为流水号
    const unsigned long long ullSequenceBit =              0xFFFF;
    const unsigned long long ullWorkerIndexBit =         0xFF0000;
    const unsigned long long ullNodeIndexBit =         0xFF000000;
    const unsigned long long ullTimeIndexBit = 0xFFFFFFFF00000000;

    static unsigned int uiSequence = 0;
    unsigned long long ullTime = ::time(NULL);//1484276747
    ++uiSequence;
    uiSequence &= ullSequenceBit;
    //4294967296    256   256   65536(0xFFFF)
    uint64 ullUniqueId = (uint64(ullTime << 32) & ullTimeIndexBit) | \
                    (uint64(uiNodeId << 24) & ullNodeIndexBit) | (uint64(ucWorkerIndex << 16) & ullWorkerIndexBit) | \
                    (uint64(uiSequence) & ullSequenceBit);
    return ullUniqueId;
}

//毫秒级时间戳id
inline uint64 GetUniqueId_MS(unsigned short uiNodeId, unsigned char ucWorkerIndex)
{
	//1bit未使用（可以兼容64位整形）  41bit作为毫秒数，11bit作为机器的ID（7个bit是节点，4个bit的工作者id），11bit作为流水号
	const unsigned long long ullSequenceBit =              0x07FF;//11bit 2048 ullSequenceBit
	const unsigned long long ullWorkerIndexBit =         0x008800;//4bit 16 ucWorkerIndex
	const unsigned long long ullNodeIndexBit =         0x003F7000;//7bit 128 uiNodeId
	const unsigned long long ullTimeIndexBit = 0x7FFFFFFFFFC00000;//41bit 2199023255552 ullTime

	static unsigned int uiSequence = 0;
	std::chrono::milliseconds timeNow = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
	unsigned long long ullTime = timeNow.count();
	//1579155560156
	//2199023255552
	++uiSequence;
	uiSequence &= ullSequenceBit;
	//1bit  41 bit   7 bit 4 bit 11bit
	//1   2199023255552    128   16   2048
	uint64 ullUniqueId = (uint64(ullTime << 22) & ullTimeIndexBit) | \
					(uint64(uiNodeId << 15) & ullNodeIndexBit) | (uint64(ucWorkerIndex << 11) & ullWorkerIndexBit) | \
					(uint64(uiSequence) & ullSequenceBit);
	return ullUniqueId;
}

//大集群毫秒级时间戳id
inline uint64 GetUniqueId_LG(unsigned short uiNodeId, unsigned char ucWorkerIndex)
{
	//1bit未使用（可以兼容64位整形）  41bit作为毫秒数，14bit作为机器的ID（8个bit是节点，6个bit的工作者id），8bit作为流水号
	const unsigned long long ullSequenceBit =              0x00FF;//8bit 256 ullSequenceBit
	const unsigned long long ullWorkerIndexBit =         0x003F00;//6bit 64 ucWorkerIndex
	const unsigned long long ullNodeIndexBit =         0x003FC000;//8bit 256 uiNodeId
	const unsigned long long ullTimeIndexBit = 0x7FFFFFFFFFC00000;//41bit 2199023255552 ullTime

	static unsigned int uiSequence = 0;
	std::chrono::milliseconds timeNow = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
	unsigned long long ullTime = timeNow.count();
	//1579155560156
	//2199023255552
	++uiSequence;
	uiSequence &= ullSequenceBit;
	//1bit  41 bit   8 bit 6 bit 8bit
	//1   2199023255552    256   128   256(0xFFFF)
	uint64 ullUniqueId = (uint64(ullTime << 22) & ullTimeIndexBit) | \
					(uint64(uiNodeId << 14) & ullNodeIndexBit) | (uint64(ucWorkerIndex << 8) & ullWorkerIndexBit) | \
					(uint64(uiSequence) & ullSequenceBit);
	return ullUniqueId;
}

inline uint32 GetMsgSeq_S()
{
	//11bit作为秒数的低位(2047秒，约34分钟)，21bit作为流水号（2097153, 1FFFFF）,共32位 0xFFFFFFFF
	const unsigned int ullSequenceBit =  0x1FFFFF;
	const unsigned int ullTimeIndexBit = 0xFFE00000;
	static unsigned int uiSequence = 0;
	unsigned int uiTime = ::time(NULL);//1484276747
	++uiSequence;
	uiSequence &= ullSequenceBit;
	unsigned int uiMsgSeq = (uint32(uiTime << 21) & ullTimeIndexBit) | (uint32(uiSequence) & ullSequenceBit);
	return uiMsgSeq;
}

inline uint32 GetMsgSeq_S(unsigned int uiTime)
{
	//11bit作为秒数的低位(2047秒，约34分钟)，21bit作为流水号（2097153, 1FFFFF）,共32位 0xFFFFFFFF
	const unsigned int ullSequenceBit =  0x1FFFFF;
	const unsigned int ullTimeIndexBit = 0xFFE00000;
	static unsigned int uiSequence = 0;
	++uiSequence;
	uiSequence &= ullSequenceBit;
	unsigned int uiMsgSeq = (uint32(uiTime << 21) & ullTimeIndexBit) | (uint32(uiSequence) & ullSequenceBit);
	return uiMsgSeq;
}

inline uint32 GetMsgSeq_MS()
{
	//21bit作为毫秒的低位(2097153毫秒，约34分钟)，11bit作为流水号（2047, 7FF）,共32位 0xFFFFFFFF
	const unsigned int ullSequenceBit =  0x7FF;
	const unsigned int ullTimeIndexBit = 0xFFFFF800;
	static unsigned int uiSequence = 0;
	//取低位
	unsigned int uiTime = (unsigned int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	++uiSequence;
	uiSequence &= ullSequenceBit;
	unsigned int uiMsgSeq = (uint32(uiTime << 11) & ullTimeIndexBit) | (uint32(uiSequence) & ullSequenceBit);
	return uiMsgSeq;
}

inline uint32 GetMsgSeq_MS(unsigned long long ullTime)
{
	//21bit作为毫秒的低位(2097153毫秒，约34分钟)，11bit作为流水号（2047, 7FF）,共32位 0xFFFFFFFF
	const unsigned int ullSequenceBit =  0x7FF;
	const unsigned int ullTimeIndexBit = 0xFFFFF800;
	static unsigned int uiSequence = 0;
	//取低位
	unsigned int uiTime = (unsigned int)ullTime;
	++uiSequence;
	uiSequence &= ullSequenceBit;
	unsigned int uiMsgSeq = (uint32(uiTime << 11) & ullTimeIndexBit) | (uint32(uiSequence) & ullSequenceBit);
	return uiMsgSeq;
}

/*
 生成随机密钥
 num： 随机密钥长度（最大长度128）
 例如: GetPassword(10)
 返回 :XZ4578F74B
*/
inline std::string GetPassword(unsigned int size)
{
	srand(time(nullptr));
	if (size > 128) size = 128;//限制最大长度
	std::string s;
	for(unsigned int i= 0; i< size; i++)
	{
		char ch(0);
		int num = rand()%3;
		if(num == 0)
			ch = static_cast<char>('0' + rand()%('9'-'0'+1));//getDigit
		else if(num == 1)
			ch = static_cast<char>('a' + rand()%('z'-'a'+1));//getLower
		else
			ch = static_cast<char>('A' + rand()%('Z'-'A'+1));//getUpper
		s.push_back(ch);
	}
	return s;
}


/*
 分割字符串
 strSrc：被分割字符串
 num：分割的个数（如果分割得不能再分割了，也会结束分割）
 c：
  返回 :vec
*/
inline void StringSplit(const std::string& strSrc,unsigned int num,std::vector<std::string>& vec,char c= ':')
{
	if (strSrc.size() > 0)
	{
		unsigned int iPosBegin = 0;
		unsigned int iPosEnd = 0;
		for (unsigned int iSegNo = 0; iSegNo < num; ++iSegNo)
		{
			iPosEnd = strSrc.find(c, iPosBegin);
			if (iPosEnd > iPosBegin)
			{
				vec.push_back(strSrc.substr(iPosBegin, iPosEnd - iPosBegin));
			}
			else
			{
				break;
			}
			iPosBegin = iPosEnd + 1;
		}
	}
}

}//namespace util

#endif /* CODE_LOSS_SRC_UTIL_COMMONUTILS_HPP_ */
