#ifndef SRC_CMDRAFTAPPENDENTRIES_HPP_
#define SRC_CMDRAFTAPPENDENTRIES_HPP_

#include "Comm.hpp"
#include "SessionRaftCluster.hpp"

namespace coor
{

class CmdRaftAppendEntries : public net::Cmd
{
public:
    CmdRaftAppendEntries() = default;
    ~CmdRaftAppendEntries() override = default;
    bool Init() override;
    bool AnyMessage(const net::tagMsgShell &stMsgShell, const MsgHead &oMsgHead, const MsgBody &oMsgBody) override;

private:
    SessionRaftCluster *m_pSessionRaft = nullptr;
};

} // namespace coor

#endif
