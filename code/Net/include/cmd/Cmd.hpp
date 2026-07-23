/*******************************************************************************
 * Project:  Net
 * @file     Cmd.hpp
 * @brief    业务处理基类
 * @author   cjy
 * @date:    2016年12月9日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef CMD_HPP_
#define CMD_HPP_
#include "../NetDefine.hpp"
#include "../NetError.hpp"

#include "labor/Labor.hpp"
#include "step/Step.hpp"
#include "step/StepNode.hpp"

namespace net
{

class Step;

}

namespace util
{

class CJsonObject;

}

namespace net
{

/**
 * @brief 指令基类
 */
class Cmd
{
public:
    Cmd();
    virtual ~Cmd();
    /**
     * @brief 初始化Cmd
     * @note Cmd类实例初始化函数，大部分Cmd不需要初始化，需要初始化的Cmd可派生后实现此函数，
     * 在此函数里可以读取配置文件（配置文件必须为json格式）。配置文件由Cmd的设计者自行定义，
     * 存放于conf/目录，配置文件名最好与Cmd名字保持一致，加上.json后缀。配置文件的更新同步
     * 会由框架自动完成。
     * @return 是否初始化成功
     */
    virtual bool Init(){return(true);}
    /**
     * @brief 命令处理入口
     * @note 框架层成功解析数据包后，根据MsgHead里的Cmd找到对应的Cmd类实例调用将数据包及
     * 数据包来源MsgShell传给AnyMessage处理。若处理过程不涉及网络IO之类需异步处理的耗时调
     * 用，则无需新创建Step类实例来处理。若处理过程涉及耗时异步调用，则应创建Step类实例，
     * 并向框架层注册Step类实例，调用Step.Start()后即返回。
     * @param stMsgShell 消息外壳
     * @param oInMsgHead 数据包头
     * @param oInMsgBody 数据包体
     * @return 命令是否处理成功
     */
    virtual bool AnyMessage(const tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody) = 0;
    // ↓ 新虚函数只能追加在既有虚函数末尾, 插到中间会位移全部 vtable 槽位 (旧 .so ABI 不兼容)
    /**
     * @brief 是否 net::Module 派生类
     * @note 跨 SO 边界禁止用 static_cast<Module*> 探测类型 (纯 Cmd 对象更小, 越界写 UB),
     * 统一用此虚函数判别. 纯 Cmd 返回 false.
     */
    virtual bool IsModule() const {return(false);}
    /**
     * @brief 设置模块配置
     * @note 只有 Module 子类持有 m_oModuleConf; 纯 Cmd (如 CmdGetToken) 无此成员, 默认忽略.
     */
    virtual void SetModuleConf(const util::CJsonObject& oConf) {(void)oConf;}
    /**
     * @brief 获取指令号码
     */
	int GetCmd() const{return(m_uiCmd);}
	/**
	 * @brief 设置指令号码
	 */
	void SetCmd(int iCmd){m_uiCmd = iCmd;}
    const std::string& ClassName() const{return(m_strClassName);}
protected:
    void SetClassName(const std::string& strClassName){m_strClassName = strClassName;}
protected:
    char m_pErrBuff[gc_iErrBuffLen];
	net::uint32 m_uiCmd;
private:
    std::string m_strClassName;
};

} /* namespace net */

#endif /* CMD_HPP_ */
