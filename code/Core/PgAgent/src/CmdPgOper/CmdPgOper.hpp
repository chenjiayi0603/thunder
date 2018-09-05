/*******************************************************************************
 * Project:  DbAgent
 * @file     CmdPgOper.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月28日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDDBOPER_CMDDBOPER_HPP_
#define SRC_CMDDBOPER_CMDDBOPER_HPP_
#include "postgresql/pqxx/pqxx"
#include "dbi/MysqlDbi.hpp"
#include "util/json/CJsonObject.hpp"
#include "cmd/Cmd.hpp"
#include "storage/dataproxy.pb.h"

#ifdef __cplusplus
extern "C" {
#endif
    net::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace net
{

const int gc_iMaxBeatTimeInterval = 30;
const int gc_iMaxColValueSize = 65535;

//数据库连接结构体定义
struct tagConnection
{
    pqxx::connection* pPgConn;
    time_t ullBeatTime;
    int iQueryPermit;
    int iTimeout;

    tagConnection() : pPgConn(NULL), ullBeatTime(0), iQueryPermit(0), iTimeout(0)
    {
    }

    ~tagConnection()
    {
        if (pPgConn != NULL)
        {
            delete pPgConn;
            pPgConn = NULL;
        }
    }
};

class CmdPgOper: public Cmd
{
public:
    CmdPgOper();
    virtual ~CmdPgOper();

    virtual bool Init();

    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);

protected:
    bool GetDbConnection(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
    		const DataMem::MemOperate& oQuery, pqxx::connection** ppMasterDbi, pqxx::connection** ppSlaveDbi);
    bool FetchOrEstablishConnection(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,
    				DataMem::MemOperate::DbOperate::E_QUERY_TYPE eQueryType,
                    const std::string& strMasterIdentify, const std::string& strSlaveIdentify,
                    const util::CJsonObject& oInstanceConf, pqxx::connection** ppMasterDbi, pqxx::connection** ppSlaveDbi);
    std::string GetFullTableName(const std::string& strTableName, uint64 uiFactor);

    int ConnectDb(const util::CJsonObject& oInstanceConf, pqxx::connection* &pPgConn,const std::string& strDbIdentify);
    int ConnectDb(const util::tagDbConfDetail &stDbConfDetail, pqxx::connection* &pPgConn);
    int SyncQuery(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn);
    void CheckConnection(); //检查连接是否已超时
    void Response(const net::tagMsgShell &stMsgShell,const MsgHead &oInMsgHead,int iErrno, const std::string& strErrMsg);
    bool Response(const net::tagMsgShell &stMsgShell,const MsgHead &oInMsgHead,const DataMem::MemRsp& oRsp);

    bool CreateSql(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strSql);
    bool CreateSelect(const DataMem::MemOperate& oQuery, std::string& strSql);
    bool CreateInsert(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strSql);
    bool CreateUpdate(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strSql);
    bool CreateDelete(const DataMem::MemOperate& oQuery, std::string& strSql);
    bool CreateCondition(const DataMem::MemOperate::DbOperate::Condition& oCondition, pqxx::connection* pPgConn, std::string& strCondition);
    bool CreateConditionGroup(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strCondition);
    bool CreateGroupBy(const DataMem::MemOperate& oQuery, std::string& strGroupBy);
    bool CreateOrderBy(const DataMem::MemOperate& oQuery, std::string& strOrderBy);
    bool CreateLimit(const DataMem::MemOperate& oQuery, std::string& strLimit);
    bool CheckColName(const std::string& strColName);
public:
    std::map<std::string, tagConnection*> m_mapDbiPool;     //数据库连接池，key为identify（如：192.168.18.22:3306）
    std::map<std::string, std::set<tagConnection*> > m_mapDBConnectSet;//数据库连接缓存 strInstance => set (DBConnect)
private:
    void RemoveFlag(std::string &str, char flag)const
    {
        std::string::iterator it = std::remove(str.begin(), str.end(), flag);
        str.erase(it, str.end());
    }
    int m_iConnectionTimeout;   //空闲连接超时（单位秒）
    std::string m_strColValue;         //字段值
    util::CJsonObject m_oDbConf;
    uint32 m_uiSectionFrom;
    uint32 m_uiSectionTo;
    uint32 m_uiHash;
    uint32 m_uiDivisor;
    std::map<std::string, std::set<uint32> > m_mapFactorSection; //分段因子区间配置，key为因子类型
    std::map<std::string, util::CJsonObject*> m_mapDbInstanceInfo;  //数据库配置信息key为("%u:%u:%u", uiDataType, uiFactor, uiFactorSection)

    uint8 m_uiSync;
};

} /* namespace net */

#endif /* SRC_CMDDBOPER_CMDDBOPER_HPP_ */
