#ifndef SRC_CMDRAFTREQUESTVOTE_HPP_
#define SRC_CMDRAFTREQUESTVOTE_HPP_

#include "Comm.hpp"
#include "SessionRaftCluster.hpp"

namespace coor
{

class CmdRaftRequestVote : public net::Cmd
{
public:
    CmdRaftRequestVote() = default;
    ~CmdRaftRequestVote() override = default;
    bool Init() override;
    bool AnyMessage(const net::tagMsgShell &stMsgShell, const MsgHead &oMsgHead, const MsgBody &oMsgBody) override;

private:
    SessionRaftCluster *m_pSessionRaft = nullptr;
};

} // namespace coor

#endif
