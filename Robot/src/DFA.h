#ifndef SRC_DFA_DEFINE_H_
#define SRC_DFA_DEFINE_H_
#include <cstdlib>
#include <cstdio>
#include <string>
#include <set>
#include <vector>
#include <list>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <algorithm>
#include <time.h>
#include <tr1/unordered_map>
#include "util/json/CJsonObject.hpp"
#include "session/Session.hpp"
#include "session/Timer.hpp"
#include "util/bzhash.hpp"
#include "util/FileUtil.h"
#include "util/CodeConvert.h"
#include "user_basic.pb.h"
#include "robot_session.pb.h"
#include "behaviour_common.pb.h"
#include "Define.h"
#include "cppjieba/Jieba.hpp"

namespace robot
{

/*
 * 在文字过滤系统中，为了能够应付较高的并发，需要尽量减少计算。采用DFA算法(确定性有限状态机)基本没有什么计算，基本是状态转移。
 * 这里采用多叉树实现的DFA算法。每个utf8编码的中文字一般是3个字符，英文字母是一个字符。关键词表则是有限状态机的所有的状态。
 * 输入的句子被分成一个个字符，每个字符是一个ascii码，范围在0~255之间，以其为索引查找子树中的成员。每个子树包含256个数组成员，即最多是256个子节点，没有子节点的则为空。
 * 这里的实现查找关键字不再重复查找已查找出来的关键字的的部分内容作为起点的词。
 * */

//dfa的多叉树
#define DFA_TREE_NODE_LEN (256)
/**
 * 树节点
 * 每个节点包含一个长度为256的数组
 */
struct TreeNode
{
    bool end;//关键词的终结
    std::vector<TreeNode*> subNodes;
    TreeNode()
    {
        end = false;
    }
    ~TreeNode()
    {
        for(oss::uint16 i = 0;i < subNodes.size();++i)
        {
            delete subNodes[i];
        }
        subNodes.clear();
    }
    /**
     * 向指定位置添加节点树
     * @param index
     * @param node
     */
    bool setSubNode(oss::uint16 index,TreeNode* node)
    {
        if(subNodes.empty())
        {
            subNodes.resize(DFA_TREE_NODE_LEN,NULL);
        }
        if (index >= subNodes.size())
        {
            return false;
        }
        subNodes[index] = node;
        return true;
    }
    bool getSubNode(oss::uint16 index,TreeNode*& node)const
    {
        if (index >= subNodes.size())
        {
            return false;
        }
        if(!subNodes[index])
        {
            return false;
        }
        node = subNodes[index];
        return true;
    }
    bool isKeywordEnd()const
    {
        return end;
    }
    void setKeywordEnd(bool end = true)
    {
        this->end = end;
    }
};
/**
 * DFA确定性有限状态机。trie树
 */
class DFA
{
public:
    DFA()
    {
        rootNode = NULL;
    }
    ~DFA()
    {
        if(rootNode)
        {
            delete rootNode;
            rootNode = NULL;
        }
    }
    bool HasKeys()const
    {
        return (NULL != rootNode);
    }
    void Clear()
    {
        if(rootNode)
        {
            delete rootNode;
            rootNode = NULL;
        }
    }
    /**
     * 创建DFA关键词树(创建时会清空之前创建的树)
     * @param keywordList 关键词列表
     * return true:创建树成功  false:创建树失败
     */
    bool CreateKeysTree(const std::set<std::string> &keywordList);
    void SetLogger(log4cplus::Logger logger)
    {
        m_Logger = logger;
    }
public:
    /*
     * 过滤
     * @param tofilter 需要过滤的语句
     * @param tofilter 过滤后的语句
     * @param boSameWord 计算相同单词(true:单词计数包括相同单词，false:不同单词计数)
     * return 含关键词个数
     * */
    oss::uint16 Filter(const std::string& tofilter,std::string& filtered,bool boSameWord=true);
    /*
     * 判断是否是关键词
     * */
    bool IsKey(const std::string& word)const;
    /**
     * 搜索关键字
     *  @param 需要过滤的语句
     *  @param 返回的关键词列表
     *  return true则含关键词，false不含关键词
     */
    bool SearchKeys(const std::string& tofilter,std::vector<std::string>& Keys);
private:
    void ChangeWords(std::string& filtered,int index,int size,char letter = '*')const;
    //根节点
    TreeNode* rootNode;
    //关键词缓存
    std::string keywordBuffer;
    std::set<std::string> aiEngineWords;
    log4cplus::Logger m_Logger;
};

enum SessionAiEngineQuestionsStatus
{
    eSessionAiEngineQuestions_start = 1,
    eSessionAiEngineQuestions_loading = 2,
    eSessionAiEngineQuestions_loaded = 3,
};

struct AppendAiEngineWords
{
    std::string word;
    std::string dir;
    uint32 wordCounter;//词频
    AppendAiEngineWords()
    {
        wordCounter = 1;
    }
    bool operator < (const AppendAiEngineWords& oWord) const
    {
        return word < oWord.word;
    }
};

struct AppendAiEngineQuestion
{
    ai_engine_question question;
};

/*
 基于规则的分词方法
分词算法--单词索引树
基于TRIE索引树（又称单词索引树 http://baike.baidu.com/view/1436495.htm）的逐字匹配算法,是建立在树型词典机制上，
匹配的过程是从索引树的根结点依次同步匹配待查词中的每个字，可以看成是对树 某一分枝的遍历。
每个问题搜索出所有的分词，把所有的分词进行哈希并保存到map表，map表的键为分词的哈希值，哈希表的值为文档的内容，每个分词对应多个文档。
在程序启动是加载所有的引擎问题的文档内容（包括问题和答案）（问题和其内容保存在redis和mysql）。
修改知识库问题时，会增量发送到搜索引擎，并在下一个定时器触发时重建索引。每隔较长一段时间也会重新全量获取引擎问题的文档内容，并重建索引。
定期检查文档是否需要重建，记录最近重建分词map表时间。

词库来源于搜狗词库
在程序启动时，加载所有的词库到单词索引树，会消耗一定的内存建立常驻内存的单词索引树。

词库停用词
属于停用词的单词，不会被加载到单词索引树。


问题检索 最大匹配方案
每个请求的问题搜索出所有的分词，每个分词获取其对应的一个或者多个问题的文档，并把所有的结果进行合并，被匹配文档id最多的则选择为相关度最大的文档，并返回其对应的文档内容。

问题检索 编辑距离方案
对于被匹配分词个数相同的文档，比较其引擎问题与请求问题的编辑距离，距离较近的优先级越高。

词频（需要优化）
需要根据TFIDF权值计算方式，增加词频优先级处理。


 * */

};

#endif /* SRC_LOGICSERVER_DEFINE_H_ */
