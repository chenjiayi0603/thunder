#ifndef CON_HASH_
#define CON_HASH_
#include <stdint.h>
#include <string>
#include <map>

typedef unsigned int QWORD;
typedef unsigned short WORD;


//字符串哈希函数
uint32_t FNVHash(const std::string& str)
{
   const uint32_t fnv_prime = 0x811C9DC5;
   uint32_t hash = 0;
   for(std::size_t i = 0; i < str.length(); i++)
   {
      hash *= fnv_prime;
      hash ^= str[i];
   }

   return hash;
}

typedef uint32_t (*HashFunction)(const std::string& str);

struct ServerEntry
{
	int wdServerID;
	std::string pstrIP;
	int wdPort;
	int dwInuse;
};

struct ServerInfo
{
	ServerInfo(int ID,const std::string& IP,int Port):wdServerID(ID),pstrIP(IP),wdPort(Port){}
	int wdServerID;
	std::string pstrIP;
	int wdPort;
	std::string getAddrString()const
	{return pstrIP + ":" + std::to_string(wdPort) + "#" +  std::to_string(wdServerID);};
	const std::string getServerID(){return wdServerID;}
};

struct zNode
{
	static int kVirtualNodeDefault = 80;
	zNode(const std::string& iden,	uint32_t count,	void *data):
		node_iden(iden),virtual_node_count(count),nodedata(data){}
	int getVNodeCount()const {return zNode::kVirtualNodeDefault;}
	const std::string& getNodeIden()const {return node_iden;}
	void * getData(){return nodedata;}
	std::string node_iden;
	uint32_t virtual_node_count;
	void *nodedata;
};
struct zVirtualNode
{
	zVirtualNode(const std::string& iden,	uint32_t count,	void *data=NULL):
		node_iden(iden),virtual_node_count(count),nodedata(data){}
	zNode * getNode()const {return nodedata;}
	uint32_t getHash()const {return FNVHash(node_iden);}
	std::string node_iden;
	uint32_t virtual_node_count;
	void *nodedata;
};


class zConHash
{
public:
HashFunction hash_func_ ;
typedef std::map<uint32_t,zVirtualNode> VNodeMap;
typedef VNodeMap::const_iterator VNodeMap_CIT;
typedef VNodeMap::iterator VNodeMap_IT;
typedef VNodeMap<uint32_t,zVirtualNode>::value_type  VNodeMap_VT;


typedef std::list<zNode*> NodeList;
typedef NodeList::const_iterator NodeList_CIT;
typedef NodeList::iterator NodeList_IT;

static int kVirtualNodeDefault = 80;

VNodeMap vnode_map_;
NodeList node_list_;

//一致性哈希列表
zConHash()
    :hash_func_(NULL)
{
}
//删除服务器实节点和虚节点
~zConHash()
{
    VNodeMap_IT it = vnode_map_.begin();
    for (; it!=vnode_map_.end(); ++it)
    {
        delete it->second;
    }
    vnode_map_.clear();

    NodeList_IT nodeit = node_list_.begin();
    for (; nodeit!=node_list_.end(); ++nodeit)
    {
        delete (*nodeit);
    }
    node_list_.clear();
}

//设置哈希函数
void setHashFunc(HashFunction func)
{
    hash_func_ = func;
}

//添加节点（传输地址、虚节点数量、实节点）
int addNode(const std::string &node_iden, uint32_t virtual_node_count, void *nodedata)
{
//    zBaseGlobal::logger->debug("%s(iden=%s, vcount=%u)", __FUNCTION__, node_iden.c_str(), virtual_node_count);
    if (node_iden=="" || virtual_node_count==0)
        return -1;
    zNode *pnode = new zNode(node_iden, virtual_node_count, nodedata);
    if (pnode == NULL)
        return -1;
    node_list_.push_back(pnode);

    for (uint32_t i=0; i<pnode->getVNodeCount(); i++)
    {
        char vstr[16] = {0};
        snprintf(vstr, sizeof(vstr)-1, "#%u", i);
        std::string vnodestr = pnode->getNodeIden() + vstr;
        uint32_t vhash = hash_func_(vnodestr);//哈希服务器虚节点
        zVirtualNode *vnode = new zVirtualNode(pnode, vhash);
        if (vnode == NULL)
            return -1;
        vnode_map_.insert(VNodeMap_VT(vhash, vnode));
    }
    return 0;
}

//移除节点
int removeNode(const std::string &node_iden)
{
    NodeList_IT nodeit = node_list_.begin();
    for (; nodeit!=node_list_.end(); ++nodeit)
    {
        if ((*nodeit)->getNodeIden() == node_iden)//移除实节点
            break;
    }
    if (nodeit == node_list_.end())
        return 0;
    zNode *pnode = *nodeit;

    VNodeMap_IT it = vnode_map_.begin();
    while (it != vnode_map_.end())
    {
        if (it->second->getNode() == pnode)//移除虚节点
        {
            delete (it->second);
            vnode_map_.erase(it++);
        }
        else
        {
            ++it;
        }
    }
    delete pnode;
    node_list_.erase(nodeit);
    return 0;
}

//移除所有实节点和虚节点
void clear()
{
    VNodeMap_IT it = vnode_map_.begin();
    for (; it!=vnode_map_.end(); ++it)
    {
        delete it->second;
    }
    vnode_map_.clear();

    NodeList_IT nodeit = node_list_.begin();
    for (; nodeit!=node_list_.end(); ++nodeit)
    {
        delete (*nodeit);
    }
    node_list_.clear();
}

//根据缓存内容（频道ID）获取最相近的服务器哈希值
void* lookupNode(const std::string &object) const
{
    if (vnode_map_.empty())
        return NULL;
    uint32_t objecthash = hash_func_(object);
    VNodeMap_CIT cit = vnode_map_.lower_bound(objecthash);
    if (cit == vnode_map_.end())
        cit = vnode_map_.begin();
//    zBaseGlobal::logger->debug("%s(%s)-->hash=%u->%s", __FUNCTION__,object.c_str(), objecthash, cit->second->getNode()->getNodeIden().c_str());
    return cit->second->getNode()->getData();
}

//输出所有实节点和虚节点的列表
void dump() const
{
    NodeList_CIT nodecit = node_list_.begin();
    for (; nodecit!=node_list_.end(); ++nodecit)//遍历实节点列表
    {
        zNode *pnode = *nodecit;
		std::ostringstream vnodess;

        VNodeMap_CIT cit = vnode_map_.begin();
        for (; cit!=vnode_map_.end(); ++cit)//虚节点列表
        {
            if (cit->second->getNode() == pnode)
            {
                vnodess << cit->second->getHash() << " ";
            }
        }
//        zBaseGlobal::logger->debug("[node]%s--[vnodecount=%u]:%s", pnode->getNodeIden().c_str(), pnode->getVNodeCount(),vnodess.str().c_str());
    }
}




class zChannelConHash
{
public:
typedef std::map<int,ServerEntry> ServerInfoMap;
typedef ServerInfoMap::const_iterator ServerInfoMap_CIT;
typedef ServerInfoMap::iterator ServerInfoMap_IT;
typedef std::make_pair ServerInfoMap_VT;
ServerInfoMap server_info_map_;
zConHash proxy_;
void setHashFunc(HashFunction func):

//频道哈希列表
zChannelConHash()
{
    setHashFunc(&FNVHash);//设置哈希函数
}

~zChannelConHash()
{
    ServerInfoMap_IT it = server_info_map_.begin();
    for (; it!=server_info_map_.end(); ++it)
        delete (it->second);
    server_info_map_.clear();
}
//加入新服务器节点到哈希列表
int addNode(const ServerEntry &entry)
{
    if (entry.dwInuse==0)
        return -1;
    ServerInfo *pnewinfo = new ServerInfo(entry.wdServerID, entry.pstrIP, entry.wdPort);// 服务器信息
    if (pnewinfo == NULL)
        return -1;
    server_info_map_.insert(ServerInfoMap_VT(entry.wdServerID,pnewinfo));//加入服务器节点到列表

    return proxy_.addNode(pnewinfo->getAddrString(), zConHash::kVirtualNodeDefault,pnewinfo);
}

//移除服务器对应的节点
int removeNode(WORD serverid)
{
    ServerInfoMap_IT it = server_info_map_.find(serverid);
    if (it == server_info_map_.end())
        return 0;
    std::string serveripstring = it->second->getAddrString();

    int status = proxy_.removeNode(serveripstring);
    if (status < 0)
        return status;

    delete (it->second);
    server_info_map_.erase(it);
    return 0;
}

//删除服务器列表信息
void clear()
{
    proxy_.clear();

    ServerInfoMap_IT it = server_info_map_.begin();
    for (; it!=server_info_map_.end(); ++it)
        delete (it->second);
    server_info_map_.clear();
}
//由频道ID（缓存内容）哈希获取服务器ID
WORD lookupNode(QWORD channelid) const
{
    if (channelid == 0)
        return 0;
    char channelidstr[32] = {0};
    snprintf(channelidstr, sizeof(channelidstr)-1, "%lu", channelid);

    void *pnodedata = proxy_.lookupNode(std::string(channelidstr));
    if (pnodedata == NULL)
        return 0;
    ServerInfo *pinfo = static_cast<ServerInfo*>(pnodedata);
    return pinfo->getServerID();
}
//获取服务器ID列表
uint32_t getServerIDList(std::list<WORD> &idlist) const
{
    ServerInfoMap_CIT cit = server_info_map_.begin();
    for (; cit!=server_info_map_.end(); ++cit)
    {
        idlist.push_back(cit->first);
    }
    return idlist.size();
}

};



};

#endif//ifndef
