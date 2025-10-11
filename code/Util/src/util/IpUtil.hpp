#ifndef __CUSTOM_IP_UTIL_H__
#define __CUSTOM_IP_UTIL_H__

#include <string>
#include <vector>

namespace util
{

//bool GetAllInterfaceInfoVec(std::vector<std::string> &vecIPs,bool boNoNeedLocalIp);

bool GetHostName(std::string& hostName);

//bool GetFirstHostInfo(std::string& hostName, std::string& Ip,bool boNeedIpv6);

bool GetAllHostInfo(std::string& hostName, std::vector<std::string> &vecIPs,bool boNeedIpv6);

bool AutoGetFirstHostInfo(std::string &strIp);

bool GetIpbyName(const std::string &name,std::string &strIp);

bool DomainToIP(const std::string& host, const char *portStr, std::string& strIp, std::string& strError);//建议用本接口获取ip

bool LocalDomainToIP(std::string& strIp);

}

#endif /* __CUSTOM_IP_UTIL_H__ */

