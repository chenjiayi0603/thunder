/*******************************************************************************
 * Project:  Net
 * @file     Nodes.hpp
 * @brief    节点Session
 * @author   Tommy
 * @date:    2020年1月9日
 * @note     存储节点信息，提供节点的添加、删除、修改操作，提供通过hash字符串或hash值定位具体节点操作。
 * Modify history:
 ******************************************************************************/
#ifndef SRC_IOS_NODES_HPP_
#define SRC_IOS_NODES_HPP_

#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include "util/HashCalc.hpp"
#include "NetDefine.hpp"

//#define ROT32(x, y) ((x << y) | (x >> (32 - y))) // avoid effort

namespace net
{

const unsigned long FNV_64_INIT = 0x100000001b3;
const unsigned long FNV_64_PRIME = 0xcbf29ce484222325;

enum E_HASH_ALGORITHM
{
    HASH_fnv1a_64           = 0,
    HASH_fnv1_64            = 1,
    HASH_murmur3_32         = 2,
};

/**
 * @brief 节点管理器
 * @note 存储节点信息，提供节点的添加、删除、修改操作，提供通过hash字符串或hash值定位具体节点操作。
 */
class Nodes
{
	typedef uint32_t (*HashFunction)(const char *key, size_t key_length);
public:
    /**
     * @note 节点管理Session构造函数
     * @param iVirtualNodeNum 每个实体节点对应的虚拟节点数量
     * @param dSessionTimeout 超时时间，0表示永不超时
     */
    Nodes(int iHashAlgorithm = HASH_fnv1a_64, int iVirtualNodeNum = 200);
    virtual ~Nodes();

    /* 实体节点hash信息
     * key为Identify字符串
     * value为hash(Property001#0) hash(Property001#1) hash(Property001#2) 组成的vector */
    typedef std::unordered_map<std::string, std::vector<uint32> > T_NODE2HASH_MAP;

    struct tagNode
    {
        T_NODE2HASH_MAP mapNode2Hash;
        T_NODE2HASH_MAP::iterator itPollingNode;
        std::map<uint32, std::string> mapHash2Node;

        tagNode() = default;
        ~tagNode() = default;
        tagNode(const tagNode& stNode) = delete;
        tagNode& operator=(const tagNode& stNode) = delete;
    };

public:
    /**
     * @brief 获取节点信息
     * @note 通过hash key获取一致性hash算法计算后对应的节点信息
     * @param[in] strNodeType节点类型
     * @param[in] strHashKey 数据操作的key值
     * @return strNodeIdentify 节点标识
     */
    const std::string& GetNodeIdentify(const std::string& strNodeType, const std::string& strHashKey);
    const std::string&  GetNodeIdentify(const std::string& strNodeType, uint32 uiHash);
    /**
	 * @brief 获取轮询节点信息
	 * @param[in] strNodeType节点类型
	 * @return strNodeIdentify 节点标识
	 */
    const std::string&  GetNodeIdentify(const std::string& strNodeType);
    /**
	 * @brief 获取所有节点信息
	 * @param[in] strNodeType节点类型
	 * @return strNodeIdentify 节点标识集合
	 */
    const std::unordered_set<std::string>& GetNodeIdentifys(const std::string& strNodeType);
    /**
     * @brief 添加节点
     * @note 添加节点信息，每个节点均有一个主节点一个被节点构成。
     * @param strNodeIdentify 节点标识
     */
    void AddNodeIdentify(const std::string& strNodeType, const std::string& strNodeIdentify);
    /**
     * @brief 删除节点
     * @note 删除节点信息，每个节点均有一个主节点一个被节点构成。
     * @param strNodeIdentify 节点标识
     */
    void DelNodeIdentify(const std::string& strNodeType, const std::string& strNodeIdentify);

    bool IsNodeType(const std::string& strNodeIdentify, const std::string& strNodeType);

    void SetCheckInternalRouter(bool bCheckInternalRouter,uint32 HeartbeatTime = 20);
    bool SetHeartBeat(const std::string& strNodeIdentify);
	bool CheckHeartBeat(const std::string& strNodeIdentify);
	bool IsAlive(const std::string& strNodeIdentify,bool bAutoConnent = true);
	bool IsCheckInternalRouter()const {return m_bCheckInternalRouter;}

	bool CheckIsAlive(const std::string& strNodeIdentify);
	void ClearAlive(const std::string& strNodeIdentify);//测试接口
protected:
    HashFunction m_hashfunc;
private:
    const int m_iHashAlgorithm = HASH_fnv1a_64;
    const int m_iVirtualNodeNum = 200;

    std::unordered_map<std::string,std::shared_ptr<tagNode> > m_mapNode;

    std::string m_sEmptyNodeIdentify;
    std::unordered_set<std::string> m_setEmptyIdentifySet;

    static const uint32 mc_uiBeat = 0x00000001;
	static const uint32 mc_uiAlive = 0x00000007;   ///< 最近三次心跳任意一次成功则认为在线

	bool m_bCheckInternalRouter = true;     //< 检查内部节点间路由
	uint32 m_uiHeartbeatTime = 20;
    std::unordered_map<std::string,std::pair<uint32,uint32>>  m_mapNodeIdentifyHeartbeat;//NodeIdentify:heartbeatlasttime:heartbeatbits
};

} /* namespace neb */

#endif /* SRC_IOS_NODES_HPP_ */
