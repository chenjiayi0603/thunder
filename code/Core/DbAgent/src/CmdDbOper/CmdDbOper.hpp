/*******************************************************************************
 * Project:  DbAgent
 * @file     CmdDbOper.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月28日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDDBOPER_CMDDBOPER_HPP_
#define SRC_CMDDBOPER_CMDDBOPER_HPP_
#include "dbi/MysqlDbi.hpp"
#include "util/json/CJsonObject.hpp"
#include "cmd/Cmd.hpp"
#include "storage/dataproxy.pb.h"
#include "step/StepState.hpp"
#include "step/MysqlStep.hpp"

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
    util::CMysqlDbi* pDbi;
    time_t ullBeatTime;
    int iQueryPermit;
    int iTimeout;

    tagConnection() : pDbi(NULL), ullBeatTime(0), iQueryPermit(0), iTimeout(0)
    {
    }

    ~tagConnection()
    {
        if (pDbi != NULL)
        {
            delete pDbi;
            pDbi = NULL;
        }
    }
};

class CmdDbOper: public Cmd
{
public:
    CmdDbOper();
    virtual ~CmdDbOper();

    virtual bool Init();

    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);

protected:
    bool GetDbConnection(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
    		const DataMem::MemOperate& oQuery, util::CMysqlDbi** ppMasterDbi, util::CMysqlDbi** ppSlaveDbi);
    bool FetchOrEstablishConnectionForCluster(const std::string &strInstance,util::CJsonObject& oHostListConf,
                    const util::CJsonObject& oInstanceConf, util::CMysqlDbi** ppMasterDbi);
    bool FetchOrEstablishConnection(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,
    				DataMem::MemOperate::DbOperate::E_QUERY_TYPE eQueryType,
                    const std::string& strMasterIdentify, const std::string& strSlaveIdentify,
                    const util::CJsonObject& oInstanceConf, util::CMysqlDbi** ppMasterDbi, util::CMysqlDbi** ppSlaveDbi);
    std::string GetFullTableName(const std::string& strTableName, uint64 uiFactor);

    int ConnectDb(const util::CJsonObject& oInstanceConf, util::CMysqlDbi* pDbi,const std::string& strDbIdentify);
    int ConnectDb(const util::tagDbConfDetail &stDbConfDetail, util::CMysqlDbi* pDbi);
    int SyncQuery(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const DataMem::MemOperate& oQuery, util::CMysqlDbi* pDbi);
    int AsyncQuery(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const DataMem::MemOperate& oQuery, util::CMysqlDbi* pDbi);
    int AsyncQueryCallback(const DataMem::MemOperate& oQuery, util::MysqlResSet* pMysqlResSet,int iResult,
    		const std::string &strSql,const net::tagMsgShell &stMsgShell,const MsgHead &oInMsgHead,
			uint32 uiSectionFrom,uint32 uiSectionTo,uint32 uiHash,uint32 uiDivisor);
    void CheckConnection(); //检查连接是否已超时
    void Response(const net::tagMsgShell &stMsgShell,const MsgHead &oInMsgHead,int iErrno, const std::string& strErrMsg);
    bool Response(const net::tagMsgShell &stMsgShell,const MsgHead &oInMsgHead,const DataMem::MemRsp& oRsp);

    bool CreateSql(const DataMem::MemOperate& oQuery, util::CMysqlDbi* pDbi, std::string& strSql);
    bool CreateSelect(const DataMem::MemOperate& oQuery, std::string& strSql);
    bool CreateInsert(const DataMem::MemOperate& oQuery, util::CMysqlDbi* pDbi, std::string& strSql);
    bool CreateUpdate(const DataMem::MemOperate& oQuery, util::CMysqlDbi* pDbi, std::string& strSql);
    bool CreateDelete(const DataMem::MemOperate& oQuery, std::string& strSql);
    bool CreateCondition(const DataMem::MemOperate::DbOperate::Condition& oCondition, util::CMysqlDbi* pDbi, std::string& strCondition);
    bool CreateConditionGroup(const DataMem::MemOperate& oQuery, util::CMysqlDbi* pDbi, std::string& strCondition);
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
    char* m_szColValue;         //字段值
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
