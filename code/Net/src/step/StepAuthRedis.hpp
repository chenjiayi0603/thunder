#ifndef SRC_CMDDATAPROXY_StepAuthRedis_HPP_
#define SRC_CMDDATAPROXY_StepAuthRedis_HPP_
#include <string>
#include "step/RedisStep.hpp"
#include "storage/dataproxy.pb.h"
//#include "ProtoError.h"

namespace net
{

class StepAuthRedis: public net::RedisStep
{
public:
    StepAuthRedis(const DataMem::MemOperate::RedisOperate& oRedisOperate,tagRedisAttr*& ptagRedisAttr);
    virtual ~StepAuthRedis() = default;
    virtual net::E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    virtual net::E_CMD_STATUS Callback(const redisAsyncContext *c,int status,redisReply* pReply);
private:
    void Build();
    DataMem::MemOperate::RedisOperate m_oRedisOperate;
    int m_iEmitNum = 0;
    tagRedisAttr*& m_ptagRedisAttr;
};

}

#endif
