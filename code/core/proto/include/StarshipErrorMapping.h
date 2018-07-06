/*******************************************************************************
* Project:  proto
* @file     RobotErrorMapping.h
* @brief    IM错误与系统错误映射
* @author   cjy
* @date:    2016年4月9日
* @note
* Modify history:
******************************************************************************/
#ifndef SRC_ANALYSISERRORMAPPING_H_
#define SRC_ANALYSISERRORMAPPING_H_

#include "OssError.hpp"
#include "StarshipError.h"

namespace starshiplib
{
/*
 所有 HTTP 状态代码及其定义。
　代码  指示
2xx  成功
200  正常；请求已完成。
201  正常；紧接 POST 命令。
202  正常；已接受用于处理，但处理尚未完成。
203  正常；部分信息 — 返回的信息只是一部分。
204  正常；无响应 — 已接收请求，但不存在要回送的信息。
3xx  重定向
301  已移动 — 请求的数据具有新的位置且更改是永久的。
302  已找到 — 请求的数据临时具有不同 URI。
303  请参阅其它 — 可在另一 URI 下找到对请求的响应，且应使用 GET 方法检索此响应。
304  未修改 — 未按预期修改文档。
305  使用代理 — 必须通过位置字段中提供的代理来访问请求的资源。
306  未使用 — 不再使用；保留此代码以便将来使用。
4xx  客户机中出现的错误
400  错误请求 — 请求中有语法问题，或不能满足请求。
401  未授权 — 未授权客户机访问数据。
402  需要付款 — 表示计费系统已有效。
403  禁止 — 即使有授权也不需要访问。
404  找不到 — 服务器找不到给定的资源；文档不存在。
407  代理认证请求 — 客户机首先必须使用代理认证自身。
415  介质类型不受支持 — 服务器拒绝服务请求，因为不支持请求实体的格式。
5xx  服务器中出现的错误
500  内部错误 — 因为意外情况，服务器不能完成请求。
501  未执行 — 服务器不支持请求的工具。
502  错误网关 — 服务器接收到来自上游服务器的无效响应。
503  无法获得服务 — 由于临时过载或维护，服务器无法处理请求。
-----------------------------------------------------------------------------------------------------------------------
HTTP 400 - 请求无效
HTTP 401.1 - 未授权：登录失败
HTTP 401.2 - 未授权：服务器配置问题导致登录失败
HTTP 401.3 - ACL 禁止访问资源
HTTP 401.4 - 未授权：授权被筛选器拒绝
HTTP 401.5 - 未授权：ISAPI 或 CGI 授权失败
HTTP 403 - 禁止访问
HTTP 403 - 对 Internet 服务管理器 (HTML) 的访问仅限于 Localhost
HTTP 403.1 禁止访问：禁止可执行访问
HTTP 403.2 - 禁止访问：禁止读访问
HTTP 403.3 - 禁止访问：禁止写访问
HTTP 403.4 - 禁止访问：要求 SSL
HTTP 403.5 - 禁止访问：要求 SSL 128
HTTP 403.6 - 禁止访问：IP 地址被拒绝
HTTP 403.7 - 禁止访问：要求客户证书
HTTP 403.8 - 禁止访问：禁止站点访问
HTTP 403.9 - 禁止访问：连接的用户过多
HTTP 403.10 - 禁止访问：配置无效
HTTP 403.11 - 禁止访问：密码更改
HTTP 403.12 - 禁止访问：映射器拒绝访问
HTTP 403.13 - 禁止访问：客户证书已被吊销
HTTP 403.15 - 禁止访问：客户访问许可过多
HTTP 403.16 - 禁止访问：客户证书不可信或者无效
HTTP 403.17 - 禁止访问：客户证书已经到期或者尚未生效
HTTP 404.1 - 无法找到 Web 站点
HTTP 404 - 无法找到文件
HTTP 405 - 资源被禁止
HTTP 406 - 无法接受
HTTP 407 - 要求代理身份验证
HTTP 410 - 永远不可用
HTTP 412 - 先决条件失败
HTTP 414 - 请求 - URI 太长
HTTP 500 - 内部服务器错误
HTTP 500.100 - 内部服务器错误 - ASP 错误
HTTP 500-11 服务器关闭
HTTP 500-12 应用程序重新启动
HTTP 500-13 - 服务器太忙
HTTP 500-14 - 应用程序无效
HTTP 500-15 - 不允许请求 global.asa
Error 501 - 未实现
HTTP 502 - 网关错误
 * */
inline int http_err_code(int code)
{
    switch (code)
    {
        case ERR_OK:                                      return ERR_RESPONSE_OK;
        case ERR_RESPONSE_OK:                             return ERR_RESPONSE_OK;
        case ERR_INVALID_PARAMS:                          return ERR_INVALID_PARAMS;
        case ERR_SERVER_LOGIC_ERROR:                      return ERR_SERVER_LOGIC_ERROR;
    }
    return ERR_SERVER_LOGIC_ERROR;//系统错误
}

inline const char * http_err_msg(int code)
{
    switch (code)
    {
        case ERR_OK:                                      return "ok";
        case ERR_RESPONSE_OK:                             return "ok";
        case ERR_INVALID_PARAMS:                          return "invalid parameters";
        case ERR_APPKEY_AUTHCODE:                         return "failed to auth app key";
        case ERR_SERVER_LOGIC_ERROR:                      return "logic error";
    }
    return "logic error";
}

inline int server_err_code(int code)
{
    switch (code)
    {
        case oss::ERR_OK:                               return ERR_OK;
        case oss::ERR_PARASE_PROTOBUF:                  return ERR_PARASE_PROTOBUF;
        case oss::ERR_UNKNOWN_CMD:                      return ERR_UNKNOWN_CMD;
        case oss::ERR_SERVERINFO:                       return ERR_SERVERINFO;
        case oss::ERR_BODY_JSON:                        return ERR_BODY_JSON;
        case oss::ERR_SERVERINFO_RECORD:                return ERR_SERVERINFO_RECORD;
        case oss::ERR_TIMEOUT:                          return ERR_TIMEOUT;
    }
    if (code < 10000)
    {
        return ERR_SYSTEM_ERROR;//系统错误
    }
    else if (code >= 10000 && code < 20000)  // 此种情况应在上面的switch里处理
    {
        return ERR_SERVER_ERROR;//服务器错误
    }
    else if (code >= 20000 && code < 30000)
    {
        return code;//逻辑错误
    }
    return ERR_UNKNOWN;
}

inline const char * server_err_msg(int code)
{
    switch (code)
    {
        case ERR_OK:                                    return "成功。";

        case ERR_PARASE_PROTOBUF:                       return "协议错误";
        case ERR_UNKNOWN_CMD:                           return "未知的命令字";
        case ERR_SERVERINFO:                            return "系统错误";
        case ERR_BODY_JSON:                             return "协议错误";
        case ERR_SERVERINFO_RECORD:                     return "系统错误";
        case ERR_TIMEOUT:                               return "超时";

        case ERR_UNKNOWN:                               return "未知错误[20200]";
        case ERR_UNKNOW_CMD:                            return "未知命令字[20201]";
        case ERR_SYSTEM_ERROR:                          return "系统错误[20202]";          ///< 系统错误（数据库错误等，通过查询后台Server日志可以找到更具体错误码）
        case ERR_SERVER_BUSY:                           return "系统繁忙[20203]";
        case ERR_SERVER_ERROR:                          return "服务器错误[20204]";
        case ERR_SERVER_TIMEOUT:                        return "系统繁忙，请稍后再尝试[20205]";
        case ERR_UNKNOWN_RESPONSE_CMD:                  return "未知响应命令字[20207]";    ///< 未知响应命令字，通常为对端没有找到对应Cmd处理者，由框架层返回错误
        case ERR_ASYNC_TIMEOUT:                         return "系统繁忙，请稍后再尝试[20208]";
        case ERR_LOAD_CONFIGFILE:                       return "系统错误[20209]";          ///< 模块加载配置文件失败
        case ERR_LOGIC_SERVER_TIMEOUT:                  return "系统繁忙，请稍后再尝试[20213]";    ///< 逻辑Server处理超时
        case ERR_PROC_RELAY:                            return "系统繁忙，请稍后再尝试[20214]";    ///< 数据转发到其它进程失败
        case ERR_SEND_TO_LOGIG_MSG:                     return "系统繁忙，请稍后再尝试[20215]";   ///< 发送消息到逻辑服务器失败
        case ERR_REG_SESSION:                           return "系统繁忙，请稍后再尝试[20216]";   ///< session注册失败
        case ERR_SESSION_CREATE:                        return "系统繁忙，请稍后再尝试[20217]";   ///< 创建 session 失败
        case ERR_MALLOC_FAILED:                         return "系统繁忙，请稍后再尝试[20218]";   ///< new对象失败
        case ERR_SEQUENCE:                              return "系统繁忙，请稍后再尝试[20219]";   ///< 错误序列号
        case ERR_RC5ENCRYPT_ERROR:                      return "加密失败[20220]";                ///< Rc5Encrypt 加密失败
        case ERR_RC5DECRYPT_ERROR:                      return "解密失败[20221]";                ///< Rc5Decrypt 解密失败
        case ERR_REQ_FREQUENCY:                         return "请求过于频繁[20222]";            ///< 请求过于频繁（未避免被攻击而做的系统保护）
        case ERR_NO_SESSION_ID_IN_MSGBODY:              return "数据错误[20301]";                ///< MsgBody缺少session_id
        case ERR_NO_ADDITIONAL_IN_MSGBODY:              return "数据错误[20302]";                ///< MsgBody缺少additional
        case ERR_MSG_BODY_DECODE:                       return "数据错误[20303]";                ///< msg body解析出错
        case ERR_INVALID_PROTOCOL:                      return "协议错误[20304]";                ///< 协议错误
        case ERR_PACK_INFO_ERROR:                       return "数据错误[20305]";                ///< 部分信息打成PB协议包
        case ERR_PARSE_PACK_ERROR:                      return "协议错误[20306]";                ///< 解析PB协议包
        case ERR_REQ_MISS_PB_PARAM:                     return "参数缺失[20307]";             ///< 请求缺少参数(pb中带的参数不全)
        case ERR_PROTOCOL_FORMAT:                       return "协议格式错误[20308]";             ///< 协议格式错误
        case ERR_LIST_INCOMPLETE:                       return "参数缺失[20309]";                 ///< 参数列表缺失或不全
        case ERR_BEYOND_RANGE:                          return "参数值错误[20310]";               ///< 传入的参数值超过规定范围
        case ERR_PARMS_VALUES_MISSING:                  return "参数缺失[20311]";                  ///< 传入的参数在程序传递或解析过程中丢失
        case ERR_JSON_PRASE_FAILED:                     return "协议格式错误[20312]";              ///< JSON 数据解析失败
        case ERR_INVALID_DATA:                          return "数据错误[20313]";                 ///< 错误数据
        case ERR_INVALID_SESSION_ID:                    return "协议错误[20314]";                 ///< 错误的session路由信息
        case ERR_INVALID_PARMS:                         return "无效参数[20315]";                 ///< 无效参数

        case ERR_SERVER_CONFIG_EXIST:                   return "相同更新配置已存在[28001]";//相同更新配置已存在
        case ERR_SERVER_NODE_NO_EXIST:                  return "不存在该节点[28002]";//不存在该节点
        case ERR_SERVER_NODE_ALREADY_OFFLINE:           return "该节点已下线[28003]";   ///该节点已下线
        case ERR_SERVER_NODE_ALREADY_ONLINE:            return "该节点已上线[28004]";   ///该节点已上线
        case ERR_SERVER_CENTER_RESTART_SCRIPT:          return "中心节点使用脚本关闭或重启节点[28008]";//中心节点使用脚本关闭或重启节点
        case ERR_SERVER_CENTER_NO_SUSPEND:              return "中心节点不会被挂起路由[28009]";//中心节点不会被挂起路由
        case ERR_SERVER_CENTER_NO_ROUTES_RESTORE:       return "中心节点不需要恢复路由[28010]";//中心节点不需要恢复路由
        case ERR_SERVER_LOGIC_CONFIG_NONE_RELOAD:       return "逻辑配置无需重新加载库[28011]";//逻辑配置无需重新加载库
        case ERR_SERVER_LOGIC_CONFIG_RELOAD_FAIL:       return "逻辑配置重新加载库失败[28012]";//逻辑配置重新加载库失败
        case ERR_SERVER_CENTER_NO_OPERATION:            return "中心节点不需要操作[28013]";   ///中心节点不需要操作
        case ERR_SERVER_CENTER_OPERATION_NO_TARGET:     return "中心节点操作需要指定操作节点[28014]";   ///中心节点操作需要指定操作节点
        case ERR_SERVER_NO_SUCH_ONLINE_NODE:            return "没有这个在线节点[28015]";   ///没有这个在线节点
        case ERR_SERVER_NODE_OFFLINE_NEED_MORE_NODES:   return "在线节点挂起需要更多的在线节点[28016]";//在线节点挂起需要更多的在线节点

        case ERR_SERVER_NODE_PRELOGIN_NO_GATE:          return "没有合适网关[29001]";//没有合适网关
  }

    if (code < 10000)
    {
        return "系统错误";
    }
    else if (code >= 10000 && code < 20000)  // 此种情况应在上面的switch里处理
    {
        return "服务器错误";
    }
    else if (code >= 20000 && code < 30000)  // 此种情况应在上面的switch里处理
    {
        return "逻辑错误";
    }
    return "未知错误";
}

//        case ERR_HTTP_SESSION_GET:                      return "数据错误[20401]";                 ///< HTTP SESSION 获取失败
//        case ERR_HTTP_NO_HEADER:                        return "数据错误[20402]";                 ///< http请求没有header
//        case ERR_HTTP_NO_BODY:                          return "数据错误[20403]";                 ///< http请求没有body
//        case ERR_HTTP_NO_COOKIE:                        return "Cookie缺失[20404]";               ///< http请求缺少cookie
//        case ERR_HTTP_NO_METHOD:                        return "协议格式错误[20405]";             ///< http请求缺少请求类型(GET/POST)
//        case ERR_HTTP_TYPE:                             return "协议格式错误[20406]";             ///< http类型错误(请求/响应)
//        case ERR_HTTP_METHOD:                           return "数据错误[20407]";                 ///< http接口错误(GET/POST)
//        case ERR_HTTP_PARAM_DECODE:                     return "数据错误[20408]";                 ///< http参数解析失败
//        case ERR_HTTP_PARAM_MISSING:                    return "参数缺失[20409]";                 ///< http请求缺少参数
//        case ERR_HTTP_COOKIE_PARAM_DECODE:              return "Cookie数据错误[20410]";           ///< http请求cookie参数解析失败
//        case ERR_HTTP_COOKIE_PARAM_MISSING:             return "Cookie参数缺失[20411]";           ///< http请求缺少cookie参数
//        case ERR_HTTP_PARAM_ERROR:                      return "参数格式错误[20412]";             ///< http参数格式错误
//        case ERR_HTTP_COOKIE_PARAM_ERROR:               return "Cookie数据错误[20413]";           ///< http请求Cookie参数错误
//        case ERR_HTTP_POST_TO_JAVA:                     return "系统繁忙，请稍后再尝试[20414]";    ///< http post数据给java服务时出错

}

#endif /* SRC_ANALYSISERRORMAPPING_H_ */
