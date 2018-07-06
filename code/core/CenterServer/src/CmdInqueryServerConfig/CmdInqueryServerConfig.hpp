/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdInqueryServerConfig.hpp
 * @brief   更新节点配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 * Modify history:
 ******************************************************************************/
#ifndef CMD_INQUERY_SERVER_CONFIG_HPP_
#define CMD_INQUERY_SERVER_CONFIG_HPP_
#include "protocol/oss_sys.pb.h"
#include "server.pb.h"
#include "user_basic.pb.h"
#include "cmd/Cmd.hpp"
#include "../Comm.hpp"
#include "../NodeSession.h"

namespace starshiplib
{
/**
 * @brief   检查服务器配置
 * @author  chenjiayi
 * @date    2016年8月9日
 * @note    更新节点配置
 */
class CmdInqueryServerConfig: public oss::Cmd
{
public:
    CmdInqueryServerConfig();
    virtual ~CmdInqueryServerConfig();
    virtual bool Init();
    virtual bool AnyMessage(const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead, const MsgBody& oInMsgBody);
private:
    bool parseMsg(const MsgBody& oInMsgBody,const server::user_basic &basicInfo);
    bool Response(int iRetCode);
    NodeSession* pSess;
    bool boInit;
    /*
    message node_config
    {
        //查询和更新发送
        string node_type = 1;//节点类型 ,”LOGIC”
        uint32 config_type = 2;//配置类型，0:服务器配置，其他类型为逻辑配置
        string config_content = 3;//配置内容（目前更新内容的字段名为"so"、"module"、"log_level",可更新其中之一，或者一起更新）
        uint32 auto_send = 4;//是否自动下发0：不是，1：是
        uint32 reload_config = 5;//是否在线已加载配置，0：不是，1：是
        //查询时Server发送
        string config_file  = 6;//配置文件名，如LogicServer.json
        uint32 update_time = 7;//更新时间
    }
    config_content 配置内容,如：
    {
        "so": [
            {
                "cmd": 1001,
                "so_path": "plugins/Logic/CmdLogin.so",
                "entrance_symbol": "create",
                "load": false,
                "version": 1
            }
        ],
        "module":[
            {"url_path":"/im/hello","so_path":"plugins/Interface/ModuleHello.so","entrance_symbol":"create", "load":true, "version":1}
        ],
        "log_level":0
    }
     * */
    oss::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    server::inquery_server_config_req m_oInqueryServerConfigReq;
    server::inquery_server_config_ack m_oInqueryServerConfigAck;
};
} /* namespace starshiplib */

#endif /* CMD_CHECK_SERVER_CONFIG_HPP_ */
