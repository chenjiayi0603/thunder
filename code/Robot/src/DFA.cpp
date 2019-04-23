#include "DFA.h"

namespace robot
{

bool DFA::CreateKeysTree(const std::set<std::string> &keywordSet)
{
    if(rootNode)//重新创建新的树
    {
        delete rootNode;
        rootNode = NULL;
    }
    rootNode = new TreeNode();
    //根据关键词设置节点数
    for (std::set<std::string>::const_iterator it = keywordSet.begin();
                    it != keywordSet.end();++it)
    {
        const std::string& keyword = *it;
        if(keyword.empty())
        {
            continue;
        }
        int keyLen = keyword.length();
        const char* bytes = keyword.data();//转换成ASCII字符
        TreeNode* tempNode = rootNode;
        //循环每个字节
        for (int i = 0; i < keyLen; i++)
        {
            int index = bytes[i] & 0xff; //字符转换成数字，作为索引来设置节点（ASCII字符的值是0~255）
            TreeNode* node(NULL);
            if(!tempNode->getSubNode(index,node))//没有该值对应的子节点的就设置子节点
            {
                node = new TreeNode();
                if(!tempNode->setSubNode(index, node))
                {
                    LOG4CPLUS_WARN_FMT(m_Logger,"setSubNode failed:{%s}", keyword.c_str());
                    return false;
                }
            }
            tempNode = node;
            if(i == keyLen - 1)//关键词结束， 设置节点树结束标志
            {
                tempNode->setKeywordEnd();
//              printf("DFA:{%s}len(%d)", keyword.c_str(),keyword.length());
            }
        }
    }
    return true;
}

net::uint16 DFA::Filter(const std::string& tofilter,std::string& filtered,bool boSameWord)
{
    filtered = tofilter;
    if(tofilter.empty())
    {
        LOG4CPLUS_TRACE_FMT(m_Logger,"tofilter is empty");
        return 0;
    }
    if(!rootNode)
    {
//        LOG4CPLUS_TRACE_FMT(m_Logger,"rootNode is empty");
        return 0;
    }
    keywordBuffer.clear();
    int len = tofilter.length();
    const char* bytes = tofilter.data();//转换成ASCII字符
    TreeNode* tempNode = rootNode;
    int rollback = 0;   //回滚数
    int position = 0;//当前比较的位置
    uint16 counter(0);
    aiEngineWords.clear();
    std::pair<std::set<std::string>::iterator,bool> ret;
    while (position < len)
    {
        char c = bytes[position];
        int index = c & 0xFF;
        keywordBuffer.push_back(c); //写关键词缓存
        if(!tempNode->getSubNode(index,tempNode))//当前位置的匹配结束
        {
            position -= rollback; //回退 并测试下一个字节
            rollback = 0;//不是任何关键字 回退步数 置为0
            tempNode = rootNode;//状态机复位
            keywordBuffer.clear();//清空
        }
        else if(tempNode->isKeywordEnd())//是结束点 记录关键词
        {
//                LOG4CPLUS_TRACE_FMT(m_Logger,"Find key:{%s},position(%d),size(%u),filtered size(%u)",
//                                keywordBuffer.c_str(),
//                                position,keywordBuffer.size(),filtered.size());
            ChangeWords(filtered,position - keywordBuffer.size() + 1,keywordBuffer.size());
            rollback = 1;   //遇到结束点 回退步数 置为1,开始铭感词后的下一个字节检查(这里不检查关键字内开始的可能的另一个关键字)
            if(boSameWord)
            {
                ++counter;
            }
            else
            {
                ret = aiEngineWords.insert(keywordBuffer);
                if(ret.second)
                {
                    ++counter;
                }
            }
            keywordBuffer.clear();
        }
        else
        {
            ++rollback; //非结束点 回退步数加1
        }
        ++position;
    }
    return counter;
}

bool DFA::IsKey(const std::string& word)const
{
    if(word.empty())
    {
        LOG4CPLUS_TRACE_FMT(m_Logger,"word is empty");
        return false;
    }
    if(!rootNode)
    {
        LOG4CPLUS_TRACE_FMT(m_Logger,"rootNode is empty");
        return false;
    }
    int len = word.length();
    const char* bytes = word.data();//转换成ASCII字节串
    TreeNode* tempNode = rootNode;
    int position = 0;
    for (;position  < len;++position)
    {
        char c = bytes[position];
        int index = c & 0xFF;
        if(!tempNode->getSubNode(index,tempNode))//当前位置的匹配结束
        {
            break;
        }
    }
    if(position == len)
    {
        return true;
    }
    else
    {
        return false;
    }
}


bool DFA::SearchKeys(const std::string& tofilter,std::vector<std::string>& keys)
{
    keys.clear();
    if(tofilter.empty())
    {
        LOG4CPLUS_TRACE_FMT(m_Logger,"tofilter is empty");
        return false;
    }
    if(!rootNode)
    {
//        LOG4CPLUS_TRACE_FMT(m_Logger,"rootNode is empty");
        return false;
    }
    keywordBuffer.clear();
    bool boFound(false);
    int len = tofilter.length();
    const char* bytes = tofilter.data();//转换成ASCII字符
    TreeNode* pNode = rootNode;
    int rollback = 0;   //回滚数
    int position = 0;//当前比较的位置
    //关键词缓存
    std::string keywordBuffer;
    while (position < len)
    {
        char c = bytes[position];
        int index = c & 0xFF;
        keywordBuffer.push_back(c); //写关键词缓存
        if(!pNode->getSubNode(index,pNode))//当前位置的匹配结束
        {
            position = position - rollback; //回退 并测试下一个字节
            rollback = 0;//不是任何关键字 回退步数 置为0
            pNode = rootNode;//状态机复位
            keywordBuffer.clear();//清空
        }
        else if(pNode->isKeywordEnd())//是结束点 记录关键词
        {
//          LOG4CPLUS_TRACE_FMT(m_Logger,"Find key:{%s}", keywordBuffer.c_str());
            keys.push_back(keywordBuffer);
            keywordBuffer.clear();
            rollback = 1;   //遇到结束点 回退步数 置为1,开始铭感词后的下一个字节检查(这里不检查关键字内开始的可能的另一个关键字)
            boFound = true;
        }
        else
        {
            rollback++; //非结束点 回退步数加1
        }
        position++;
    }
    return boFound;
}

void DFA::ChangeWords(std::string& filtered,int index,int size,char letter)const
{
    int i = index >= 0 ?index :0;
    int l = filtered.length();
    for(;i < l && size > 0;++i,--size)
    {
        filtered[i] = letter;
    }
}

};

