#include "ConHash.hpp"
#include "labor/Labor.hpp"
#include "labor/Worker.hpp"

void NodesMgr::AddNodeIdentify(const std::string& strNodeType, const std::string& strIdentify)
{
    LOG4_TRACE("%s(%s, %s)", __FUNCTION__, strNodeType.c_str(), strIdentify.c_str());
    int iPosIpPortSeparator = strIdentify.find(':');//strHost:strPort.workerIndex
	if (iPosIpPortSeparator == std::string::npos)
	{
		LOG4_ERROR("strIdentify error: %s", strIdentify.c_str());
		return ;
	}
	std::string strHost = strIdentify.substr(0, iPosIpPortSeparator);
	std::string strPort = strIdentify.substr(iPosIpPortSeparator + 1, std::string::npos);
	int iPort = atoi(strPort.c_str());
	if (iPort == 0)
	{
		LOG4_ERROR("strIdentify error: %s", strIdentify.c_str());
		return ;
	}
    auto iter = m_mapIdentifyNodeType.find(strIdentify);
    if (iter == m_mapIdentifyNodeType.end())
    {
        m_mapIdentifyNodeType.insert(iter,std::pair<std::string, std::string>(strIdentify, strNodeType));

        T_MAP_NODE_TYPE_IDENTIFY::iterator node_type_iter;
        node_type_iter = m_mapNodeIdentify.find(strNodeType);
        if (node_type_iter == m_mapNodeIdentify.end())
        {
            std::set<std::string,std::less<std::string> > setIdentify;
            setIdentify.insert(strIdentify);
            std::pair<T_MAP_NODE_TYPE_IDENTIFY::iterator, bool> insert_node_result;
            insert_node_result = m_mapNodeIdentify.insert(
				std::pair< std::string,std::pair<std::set<std::string>::iterator,std::set<std::string,std::less<std::string>>>>
            (strNodeType, std::make_pair(setIdentify.begin(), setIdentify)));
            //这里的setIdentify是临时变量，setIdentify.begin()将会成非法地址
            if (insert_node_result.second == false)
            {
                return;
            }
            insert_node_result.first->second.first = insert_node_result.first->second.second.begin();
        }
        else
        {
            std::set<std::string>::iterator id_iter = node_type_iter->second.second.find(strIdentify);
            if (id_iter == node_type_iter->second.second.end())
            {
                node_type_iter->second.second.insert(strIdentify);
                node_type_iter->second.first = node_type_iter->second.second.begin();
            }
        }
    }
    m_mapChannelConHash[strNodeType].addNode(ServerEntry(strIdentify,strHost,iPort));
}


void NodesMgr::DelNodeIdentify(const std::string& strNodeType, const std::string& strIdentify)
{
    LOG4_TRACE("%s(%s, %s)", __FUNCTION__, strNodeType.c_str(), strIdentify.c_str());
    auto identify_iter = m_mapIdentifyNodeType.find(strIdentify);
    if (identify_iter != m_mapIdentifyNodeType.end())
    {
        auto node_type_iter = m_mapNodeIdentify.find(identify_iter->second);
        if (node_type_iter != m_mapNodeIdentify.end())
        {
            auto id_iter = node_type_iter->second.second.find(strIdentify);
            if (id_iter != node_type_iter->second.second.end())
            {
                node_type_iter->second.second.erase(id_iter);
                node_type_iter->second.first = node_type_iter->second.second.begin();
            }
        }
        m_mapIdentifyNodeType.erase(identify_iter);
    }
    m_mapChannelConHash[strNodeType].removeNode(strIdentify);
}

const std::set<std::string>& NodesMgr::GetNodeIdentifys(const std::string& strNodeType)
{
	auto node_type_iter = m_mapNodeIdentify.find(strNodeType);
	if (node_type_iter == m_mapNodeIdentify.end())
	{
		return m_setEmptyIdentifySet;
	}
	return node_type_iter->second.second;
}


const std::string& NodesMgr::GetNodeIdentify(const std::string& strNodeType)
{
	LOG4_TRACE("%s(node_type: %s)", __FUNCTION__, strNodeType.c_str());
	auto node_type_iter = m_mapNodeIdentify.find(strNodeType);
	if (node_type_iter == m_mapNodeIdentify.end())
	{
		LOG4_ERROR("no tagMsgShell match %s!", strNodeType.c_str());
		return m_sEmptyNodeIdentify;
	}
	else
	{
		if (node_type_iter->second.first != node_type_iter->second.second.end())
		{
			std::set<std::string>::iterator id_iter = node_type_iter->second.first;
			node_type_iter->second.first++;
			return *id_iter;
		}
		else
		{
			std::set<std::string>::iterator id_iter = node_type_iter->second.second.begin();
			if (id_iter != node_type_iter->second.second.end())
			{
				node_type_iter->second.first = id_iter;
				node_type_iter->second.first++;
				return *id_iter;
			}
			else
			{
				LOG4_ERROR("no tagMsgShell match and no node identify found for %s!", strNodeType.c_str());
				return m_sEmptyNodeIdentify;
			}
		}
	}
}



