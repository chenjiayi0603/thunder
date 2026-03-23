/*******************************************************************************
 * Project:  Center
 * @file     ModuleAdmin.hpp
 * @brief    Nebula集群管理
 * @author   cjy
 * @date:    2018年12月8日
 * @note     
 * Modify history:
 ******************************************************************************/
#ifndef MODULEADMIN_HPP
#define MODULEADMIN_HPP

#include "Comm.hpp"
#include "SessionOnlineNodes.hpp"
#include "StepGetConfig.hpp"
#include "StepSetConfig.hpp"
#include "cmd/Module.hpp"

namespace coor
{

/**
 * @brief 集群管理（统一入口 /admin）
 * @note GET /admin 返回管理页（含服务列表 + JSON 命令）；GET /admin/api/services、/admin/api/service/detail 为列表与详情 API；
 *       POST /admin 将命令与参数放在 JSON body（show/get/set），与原先一致。
 * @note 页面模板路径：{工作目录}/conf/admin/AdminPage.html
 * 命令管理的JSON体格式如下：
 * {
 *     "cmd":"show",
 *     "args":["ip_white"]
 * }
 *
 * 命令帮助：
 *     show:
 *         show ip_white
 *         show subscription
 *         show subscription ${node_type}
 *         show nodes
 *         show nodes ${node_type}
 *         show node_report ${node_type}
 *         show node_report ${node_type} ${node_identify}
 *         show node_detail ${node_type}
 *         show node_detail ${node_type} ${node_identify}
 *         show center
 *     get:
 *         get node_config ${node_identify}
 *         get node_custom_config ${node_identify}
 *         get custom_config ${node_identify} ${config_file_relative_path} ${config_file_name}
 *     set:
 *         set node_config ${node_type} ${config_file_content}
 *         set node_config ${node_type} ${node_identify} ${config_file_content}
 *         set node_config_from_file ${node_type} ${config_file}
 *         set node_config_from_file ${node_type} ${node_identify} ${config_file}
 *         set node_custom_config ${node_type} ${config_content}
 *         set node_custom_config ${node_type} ${node_identify} ${config_content}
 *         set node_custom_config_from_file ${node_type} ${config_file}
 *         set node_custom_config_from_file ${node_type} ${node_identify} ${config_file}
 *         set custom_config ${node_type} ${config_file_name} ${config_file_content}
 *         set custom_config ${node_type} ${config_file_relative_path} ${config_file_name} ${config_file_content}
 *         set custom_config ${node_type} ${node_identify} ${config_file_relative_path} ${config_file_name} ${config_file_content}
 *         set custom_config_from_file ${node_type} ${config_file}
 *         set custom_config_from_file ${node_type} ${config_file_relative_path} ${config_file}
 *         set custom_config_from_file ${node_type} ${node_identify} ${config_file_relative_path} ${config_file}
 */ 
class ModuleAdmin: public net::Module
{
public:
    ModuleAdmin() = default;
    virtual ~ModuleAdmin() = default;
    virtual bool Init();
    virtual bool AnyMessage(
                    const net::tagMsgShell& stMsgShell,
                    const HttpMsg& oHttpMsg);
protected:
    void ResponseOptions(
            const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    void Show(util::CJsonObject& oCmdJson, util::CJsonObject& oResult) const;
    void Get(const net::tagMsgShell& stMsgShell,
            int32 iHttpMajor, int32 iHttpMinor,
            util::CJsonObject& oCmdJson, util::CJsonObject& oResult);
    void Set(const net::tagMsgShell& stMsgShell,
            int32 iHttpMajor, int32 iHttpMinor,
            util::CJsonObject& oCmdJson, util::CJsonObject& oResult);

private:
    void SendJsonResponse(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg,
            int status, const std::string& jsonBody);
    void SendUnifiedAdminPage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    void HandleGetServices(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    void HandleGetServiceDetail(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);

    SessionOnlineNodes* m_pSessionOnlineNodes = nullptr;
};

} /* namespace coor */

#endif /* MODULEADMIN_HPP */

