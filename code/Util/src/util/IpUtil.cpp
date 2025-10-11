
#include <unistd.h>/* gethostname */
#include <netdb.h> /* struct hostent */
#include <arpa/inet.h> /* inet_ntop */

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>

//#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
//#include <netdb.h>
//#include <unistd.h>


#include "IpUtil.hpp"

namespace util
{

//bool GetAllInterfaceInfoVec(std::vector<std::string> &vecIPs,bool boNoNeedLocalIp)
//{
//	struct ifaddrs        *ifc, *ifc1;
//	char                ip[64];
//	if (0 != getifaddrs(&ifc)) return(false);
//	ifc1 = ifc;
//
//	for(; nullptr != ifc; ifc = (*ifc).ifa_next) {
//		if (ifc->ifa_addr && ifc->ifa_addr->sa_family == AF_INET)
//		{
//			if (nullptr != (*ifc).ifa_addr) {
//				inet_ntop(AF_INET, &(((struct sockaddr_in*)((*ifc).ifa_addr))->sin_addr), ip, 64);
//				if (boNoNeedLocalIp == true && std::string(ip) != std::string("127.0.0.1"))
//				{
//					vecIPs.push_back(ip);
//				}
//			}
//		}
//	}
//	freeifaddrs(ifc1);
//	return true;
//}

bool GetHostName(std::string& hostName)
{
	char name[256];
	if (-1 == gethostname(name, sizeof(name))) //On success, zero is returned.  On error, -1 is returned, and errno is set appropriately.
	{
		return  false;
	}
	hostName = name;
	return true;
}


//bool GetFirstHostInfo(std::string& hostName, std::string& Ip,bool boNeedIpv6)
//{
//	if (!GetHostName(hostName))
//	{
//		return false;
//	}
//	struct hostent* host = gethostbyname(hostName.c_str());
//	if (nullptr == host)
//	{
//		return false;
//	}
//	char ipStr[32];
//	if ((AF_INET == host->h_addrtype && boNeedIpv6 == false) ||
//		((AF_INET == host->h_addrtype || AF_INET6 == host->h_addrtype) && boNeedIpv6 == true))
//	{
//		const char* ret = inet_ntop(host->h_addrtype, host->h_addr, ipStr, sizeof(ipStr));//first address
//		if (nullptr == ret)
//		{
//			return false;//std::cout << "hostname transform to ip failed";
//		}
//		Ip = ipStr;
//		return true;
//	}
//	return false;
//}

bool GetAllHostInfo(std::string& hostName, std::vector<std::string> &vecIPs,bool boNeedIpv6)
{
	if (!GetHostName(hostName))
	{
		return false;
	}
	struct hostent* host = gethostbyname(hostName.c_str());
	if (nullptr == host)
	{
		return false;
	}
	char *ptr, **pptr;
	char ipStr[32];
	if ((AF_INET == host->h_addrtype && boNeedIpv6 == false) ||
		((AF_INET == host->h_addrtype || AF_INET6 == host->h_addrtype) && boNeedIpv6 == true))
	{
		pptr=host->h_addr_list;//char	**h_addr_list;
		for(; *pptr != nullptr; pptr++)
		{
			const char* ret = inet_ntop(host->h_addrtype, *pptr, ipStr, sizeof(ipStr));
			if (nullptr == ret)
			{
				return false;//std::cout << "hostname transform to ip failed";
			}
			vecIPs.push_back(ipStr);
		}
	}
	return true;
}

static int ipAddressLevel(struct in_addr *addr, int offset) {
    unsigned int a = addr->s_addr;
    if ((a & 0xFF) == 10) {
        return 1 + offset;
    } else if (((a & 0xFF) == 172) && ((a >> 8 & 0xF0) == 16)) {
        return 1 + offset;
    } else if (((a & 0xFF) == 192) && ((a >> 8 & 0xFF) == 168)) {
        return 1 + offset;
    } else {
        return 3 + offset;
    }
}


bool AutoGetFirstHostInfo(std::string &ip) {
    struct ifaddrs *ifaddrs = NULL;

    if (getifaddrs(&ifaddrs)) {
        return false;
    }

    struct in_addr res = {0};

    int res_level = 0, tmp_level = 0;

    char ipBuf[64];

    struct ifaddrs *ifa;
    for (ifa = ifaddrs; NULL != ifa; ifa = ifa->ifa_next) {
        char hostname[NI_MAXHOST];
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_INET) {

            // ignore not up interface.
            if (!(ifa->ifa_flags & IFF_UP)) {
                continue;
            }

            // ignore loopback interface.
            if (ifa->ifa_flags & IFF_LOOPBACK) {
                continue;
            }

            // get hostname and ip
            if (-1 == getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), hostname, NI_MAXHOST, NULL, 0, NI_NUMERICHOST))//成功返回0,出错返回-1
            {
            	continue;
            }

            struct in_addr *addr = &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr;
            if (nullptr == inet_ntop(AF_INET, addr, ipBuf, 16)) {
                continue;
            }

            // we put interface address with a hostname in a higher priority.
            int offset = strcmp(ipBuf, hostname) == 0 ? 0 : 1;

            if (0 == res.s_addr) {
                res = *addr;
                res_level = ipAddressLevel(&res, offset);
            } else if (res_level < (tmp_level = ipAddressLevel(addr, offset))) {
                res = *addr;
                res_level = tmp_level;
            }
        }
    }
    freeifaddrs(ifaddrs);

    if (0 == res.s_addr) {
		return false;
	} else {
		if (nullptr == inet_ntop(AF_INET, &res, ipBuf, 16))
		{
			return false;
		}
		ip = ipBuf;
		return  true;
	}
}

bool GetIpbyName(const std::string &name,std::string &strIp)
{
	strIp.clear();
	struct hostent *host = nullptr;
	if( (host=gethostbyname(name.c_str())) == nullptr )
	{
		return false;
	}
//	printf("host:%p\n",host);
	char *ptr = inet_ntoa(*((struct in_addr *)host->h_addr));
	if (nullptr == ptr)
	{
		return false;
	}
	strIp = ptr;
	return true;
}

bool DomainToIP(const std::string& host, const char *portStr, std::string& strIp,std::string& strError)
{
	struct addrinfo hints, *res, *res0;
	char str[32] = {0};
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;//PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;//SOCK_STREAM
	hints.ai_protocol = IPPROTO_TCP;//IPPROTO_IP
//	hints.ai_flags    = AI_PASSIVE;

	int ret = getaddrinfo(host.c_str(), portStr, &hints, &res0);
	if (ret != 0) {
//		fprintf(stderr,"getaddrinfo: %s\n",
//				gai_strerror(ret));
		strError = gai_strerror(ret);
		return false;
	}
	bool boRet(false);
	for(res = res0; res; res = res->ai_next){
		if(res->ai_family == AF_INET){
			// Found IPv4 address
			inet_ntop(AF_INET,
					  &(((struct sockaddr_in *)(res->ai_addr))->sin_addr),
					  str, 32);
//			printf("解析出来的IPv4: %s\n", str);
			strIp = str;
			boRet = true;
			break;
		}
//		else if(res->ai_family == AF_INET6){
//			// Found IPv6 address
//			inet_ntop(AF_INET6,
//					  &(((struct sockaddr_in *)(res->ai_addr))->sin_addr),
//					  str, 32);
////			printf("解析出来的IP6: %s\n", str);
//			strIp = str;
//			boRet = true;
//			break;
//		}
	}
	if (!boRet)
	{
		strError = "no right addrinfo";
	}
	freeaddrinfo(res0);
	return boRet;
}

bool LocalDomainToIP(std::string& strIp)
{
	std::string hostName;
	if (!GetHostName(hostName))
	{
		return false;
	}
	std::string strError;
	return DomainToIP(hostName,nullptr,strIp,strError);
}


}
