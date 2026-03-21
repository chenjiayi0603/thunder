#ifndef CON_HASH_
#define CON_HASH_
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <sstream>
#include <map>
#include <list>
#include <set>
#include "NetDefine.hpp"

//字符串哈希函数
inline uint32_t ConHash_FNVHash_Func(const std::string &str) {
	const uint32_t fnv_prime = 0x811C9DC5;
	uint32_t hash = 0;
	for (std::size_t i = 0; i < str.length(); i++) {
		hash *= fnv_prime;
		hash ^= str[i];
	}

	return hash;
}


typedef uint32_t (*HashFunction)(const std::string &str);

struct ServerEntry {
	std::string pstrIP;
	int wdPort;
	std::string strIdentify;//192.168.11.66:16068.0
	int dwInuse;
	ServerEntry(const std::string &identify, const std::string &ip, int iPort,int Inuse = 0) :
			pstrIP(ip), wdPort(iPort), strIdentify(identify),dwInuse(Inuse) {
	}
	std::string getIdentify() const {
		return strIdentify;
	}
};

struct ServerInfo {
	std::string pstrIP;
	int wdPort;
	std::string strIdentify;//192.168.11.66:16068.0
	ServerInfo(const std::string &identify, const std::string &ip, int iPort) :
		pstrIP(ip), wdPort(iPort),strIdentify(identify){
	}
	std::string getIdentify() const {
		return strIdentify;
	}
};

struct ConNode {//实节点
	static const uint32_t kVirtualNodeDefault = 80;
	ConNode(const std::string &identify, uint32_t count, void *data) :
		strIdentify(identify), virtual_node_count(count), nodedata(data) {
	}
	uint32_t getVNodeCount() const {
		return ConNode::kVirtualNodeDefault;
	}
	const std::string& getIdentify() const {
		return strIdentify;
	}
	void* getData() {
		return nodedata;
	}
	std::string strIdentify;
	uint32_t virtual_node_count;
	void *nodedata;
};
struct ConVirtualNode {//虚节点
	ConVirtualNode(const std::string &identify, uint32_t count, void *data) :
		strIdentify(identify), virtual_node_count(count), nodedata(data) {
	}
	ConNode* getNode() const {
		return (ConNode*) nodedata;
	}
	uint32_t getHash() const {
		return ConHash_FNVHash_Func(strIdentify);
	}
	std::string strIdentify;
	uint32_t virtual_node_count;
	void *nodedata;
};

class ConHash {
	friend class ChannelConHash;
public:
	HashFunction hash_func_;
	typedef std::map<uint32_t, ConVirtualNode*> VNodeMap;
	typedef VNodeMap::const_iterator VNodeMap_CIT;
	typedef VNodeMap::iterator VNodeMap_IT;
	typedef VNodeMap::value_type VNodeMap_VT;

	typedef std::list<ConNode*> NodeList;
	typedef NodeList::const_iterator NodeList_CIT;
	typedef NodeList::iterator NodeList_IT;

	VNodeMap m_vnode_map;
	NodeList m_node_list;

	//一致性哈希列表
	ConHash() : hash_func_(ConHash_FNVHash_Func) {
	}
	//删除服务器实节点和虚节点
	~ConHash() {
		VNodeMap_IT it = m_vnode_map.begin();
		for (; it != m_vnode_map.end(); ++it) {
			delete it->second;
		}
		m_vnode_map.clear();
		NodeList_IT nodeit = m_node_list.begin();
		for (; nodeit != m_node_list.end(); ++nodeit) {
			delete (*nodeit);
		}
		m_node_list.clear();
	}

	//设置哈希函数
	void setHashFunc(HashFunction func) {
		hash_func_ = func;
	}

	//添加节点（Identify、虚节点数量、实节点）
	int addNode(const std::string &strIdentify, uint32_t virtual_node_count,void *nodedata) {
		//    debug("%s(iden=%s, vcount=%u)", __FUNCTION__, node_iden.c_str(), virtual_node_count);
		if (strIdentify.size() == 0 || virtual_node_count == 0) return -1;
		ConNode *pnode = new ConNode(strIdentify, virtual_node_count, nodedata);
		if (pnode == nullptr) return -1;
		m_node_list.push_back(pnode);
		for (uint32_t i = 0; i < pnode->getVNodeCount(); i++) {
			char vstr[16] = { 0 };
			snprintf(vstr, sizeof(vstr) - 1, "#%u", i);
			std::string vnodestr = pnode->getIdentify() + vstr;
			uint32_t vhash = hash_func_(vnodestr); //哈希服务器虚节点
			ConVirtualNode *vnode = new ConVirtualNode(strIdentify, vhash,pnode);
			if (vnode == nullptr)return -1;
			if(m_vnode_map.find(vhash) != m_vnode_map.end())
			{
				delete vnode;
				return -1;
			}
			m_vnode_map.insert(VNodeMap_VT(vhash, vnode));
		}
		return 0;
	}

	//移除节点
	int removeNode(const std::string &strIdentify) {
		NodeList_IT nodeit = m_node_list.begin();
		for (; nodeit != m_node_list.end(); ++nodeit) {
			if ((*nodeit)->getIdentify() == strIdentify) //移除实节点
				break;
		}
		if (nodeit == m_node_list.end()) return 0;
		ConNode *pnode = *nodeit;
		VNodeMap_IT it = m_vnode_map.begin();
		while (it != m_vnode_map.end()) {//移除虚节点
			if (it->second->getNode() == pnode){
				delete (it->second);
				m_vnode_map.erase(it++);
			} else {
				++it;
			}
		}
		delete pnode;
		m_node_list.erase(nodeit);
		return 1;
	}

	//移除所有实节点和虚节点
	void clear() {
		VNodeMap_IT it = m_vnode_map.begin();
		for (; it != m_vnode_map.end(); ++it) {
			delete it->second;
		}
		m_vnode_map.clear();

		NodeList_IT nodeit = m_node_list.begin();
		for (; nodeit != m_node_list.end(); ++nodeit) {
			delete (*nodeit);
		}
		m_node_list.clear();
	}

	//根据缓存内容（数据因子，如UID）获取最相近的服务器哈希值
	void* lookupNode(const std::string &strModFactor) const {
		if (m_vnode_map.empty()) return nullptr;
		uint32_t uiModFactorhash = hash_func_(strModFactor);
		VNodeMap_CIT cit = m_vnode_map.lower_bound(uiModFactorhash);
		if (cit == m_vnode_map.end()) cit = m_vnode_map.begin();
		//    debug("%s(%s)-->hash=%u->%s", __FUNCTION__,object.c_str(), objecthash, cit->second->getNode()->getNodeIden().c_str());
		return cit->second->getNode()->getData();
	}

	//输出所有实节点和虚节点的列表
	void dump() const {
		NodeList_CIT nodecit = m_node_list.begin();
		for (; nodecit != m_node_list.end(); ++nodecit) //遍历实节点列表
				{
			ConNode *pnode = *nodecit;
			std::ostringstream vnodess;

			VNodeMap_CIT cit = m_vnode_map.begin();//虚节点列表
			for (; cit != m_vnode_map.end(); ++cit) {
				if (cit->second->getNode() == pnode) {
					vnodess << cit->second->getHash() << " ";
				}
			}
			//        debug("[node]%s--[vnodecount=%u]:%s", pnode->getNodeIden().c_str(), pnode->getVNodeCount(),vnodess.str().c_str());
		}
	}
};
class ChannelConHash {
public:
	typedef std::unordered_map<std::string, ServerEntry> ServerInfoMap;
	typedef ServerInfoMap::const_iterator ServerInfoMap_CIT;
	typedef ServerInfoMap::iterator ServerInfoMap_IT;
	typedef ServerInfoMap::value_type ServerInfoMap_VT;
	ServerInfoMap m_server_info_map;
	ConHash m_ConHashProxy;
	HashFunction hash_func_;

	ChannelConHash() :
			hash_func_(ConHash_FNVHash_Func) {
	}
	~ChannelConHash() {
		for (ConHash::NodeList_IT nodeit = m_ConHashProxy.m_node_list.begin();
				nodeit != m_ConHashProxy.m_node_list.end(); ++nodeit) {
			if ((*nodeit)->nodedata) {
				delete static_cast<ServerInfo*>((*nodeit)->nodedata);
				(*nodeit)->nodedata = nullptr;
			}
		}
		m_server_info_map.clear();
	}
	//频道哈希列表
	void setHashFunc(HashFunction func) {
		hash_func_ = func;
	}
	//加入新服务器节点到哈希列表
	int addNode(const ServerEntry &entry) {
		if (entry.dwInuse == 1) return -1;
		// 服务器信息
		ServerInfo *pnewinfo = new ServerInfo(entry.strIdentify, entry.pstrIP,entry.wdPort);
		if (pnewinfo == nullptr) return -1;
		//加入服务器节点到列表 pnewinfo
		m_server_info_map.insert(ServerInfoMap_VT(entry.strIdentify, entry));
		return m_ConHashProxy.addNode(pnewinfo->getIdentify(),ConNode::kVirtualNodeDefault, pnewinfo);
	}

	//移除服务器对应的节点
	int removeNode(const std::string &strIdentify) {
		ServerInfoMap_IT it = m_server_info_map.find(strIdentify);
		if (it == m_server_info_map.end()) return 0;
		ServerInfo* pinfo = nullptr;
		for (ConHash::NodeList_IT nodeit = m_ConHashProxy.m_node_list.begin();
				nodeit != m_ConHashProxy.m_node_list.end(); ++nodeit) {
			if ((*nodeit)->getIdentify() == it->second.getIdentify()) {
				pinfo = static_cast<ServerInfo*>((*nodeit)->getData());
				break;
			}
		}
		int status = m_ConHashProxy.removeNode(it->second.getIdentify());
		if (status == 0) return status;
		delete pinfo;
		m_server_info_map.erase(it);
		return 0;
	}

	//删除服务器列表信息
	void clear() {
		for (ConHash::NodeList_IT nodeit = m_ConHashProxy.m_node_list.begin();
				nodeit != m_ConHashProxy.m_node_list.end(); ++nodeit) {
			if ((*nodeit)->nodedata) {
				delete static_cast<ServerInfo*>((*nodeit)->nodedata);
				(*nodeit)->nodedata = nullptr;
			}
		}
		m_ConHashProxy.clear();
		m_server_info_map.clear();
	}
	//获取strIdentify
	std::string lookupNodeIdentify(uint64_t uiModFactor) const {
		void *pnodedata = m_ConHashProxy.lookupNode(std::to_string(uiModFactor));
		if (pnodedata == nullptr) return "";
		ServerInfo *pinfo = static_cast<ServerInfo*>(pnodedata);
		return pinfo->getIdentify();
	}
	std::string lookupNodeIdentify(const std::string &strModFactor) const {
		void *pnodedata = m_ConHashProxy.lookupNode(strModFactor);
		if (pnodedata == nullptr) return "";
		ServerInfo *pinfo = static_cast<ServerInfo*>(pnodedata);
		return pinfo->getIdentify();
	}
	//获取strIdentify列表
	uint32_t getIdentifyList(std::list<std::string> &idlist) const {
		ServerInfoMap_CIT cit = m_server_info_map.begin();
		for (; cit != m_server_info_map.end(); ++cit) {
			idlist.push_back(cit->first);
		}
		return idlist.size();
	}
};

class NodesMgr
{
public:
	void AddNodeIdentify(const std::string& strNodeType, const std::string& strIdentify);
	void DelNodeIdentify(const std::string& strNodeType, const std::string& strIdentify);

	inline std::string GetNodeIdentify(const std::string& strNodeType, const std::string& strFactor)
	{
		return  m_mapChannelConHash[strNodeType].lookupNodeIdentify(strFactor);
	}
	inline std::string GetNodeIdentify(const std::string& strNodeType, uint64 uiModFactor)
	{
		return  m_mapChannelConHash[strNodeType].lookupNodeIdentify(uiModFactor);
	}
	const std::string& GetNodeIdentify(const std::string& strNodeType);
	const std::set<std::string>& GetNodeIdentifys(const std::string& strNodeType);
private:
	std::unordered_map<std::string, std::string> m_mapIdentifyNodeType;    // key为Identify，value为node_type
	typedef std::unordered_map<std::string, std::pair<std::set<std::string>::iterator, std::set<std::string> > > T_MAP_NODE_TYPE_IDENTIFY;
	T_MAP_NODE_TYPE_IDENTIFY m_mapNodeIdentify;
	std::unordered_map<std::string,ChannelConHash> m_mapChannelConHash;

	std::string m_sEmptyNodeIdentify;
	std::set<std::string> m_setEmptyIdentifySet;
};


#endif//ifndef
