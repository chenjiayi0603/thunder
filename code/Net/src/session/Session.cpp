/*******************************************************************************
 * Project:  Net
 * @file     Session.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年7月28日
 * @note
 * Modify history:
 ******************************************************************************/
#include "Session.hpp"

namespace net
{

Session::Session(uint64 ulSessionId, ev_tstamp dSessionTimeout, const std::string& strSessionClass)
    : m_dSessionTimeout(dSessionTimeout),m_strSessionClassName(strSessionClass)
{
    char szSessionId[32] = {0};
    snprintf(szSessionId, sizeof(szSessionId), "%llu", ulSessionId);
    m_strSessionId = szSessionId;
}

Session::Session(const std::string& strSessionId, ev_tstamp dSessionTimeout, const std::string& strSessionClass)
    : m_dSessionTimeout(dSessionTimeout),m_strSessionId(strSessionId), m_strSessionClassName(strSessionClass)
{
}

Session::~Session()
{
	SAFE_DELETE(m_pTimeoutWatcher)
}

} /* namespace net */
