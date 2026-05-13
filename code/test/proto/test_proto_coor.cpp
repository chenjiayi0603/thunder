/**
 * coor.proto / msg.proto 序列化单元测试
 */
#include "protocol/msg.pb.h"
#include "protocol/oss_sys.pb.h"
#include "protocol/http.pb.h"
#include "coor.pb.h"
#include <gtest/gtest.h>

TEST(ProtoMsg, MsgHeadDefault)
{
    MsgHead head;
    EXPECT_EQ(0u, head.cmd());
    EXPECT_EQ(0u, head.seq());
    EXPECT_EQ(0u, head.msgbody_len());
}

TEST(ProtoMsg, MsgHeadSetGet)
{
    MsgHead head;
    head.set_cmd(123);
    head.set_seq(456);
    head.set_msgbody_len(789);

    EXPECT_EQ(123u, head.cmd());
    EXPECT_EQ(456u, head.seq());
    EXPECT_EQ(789u, head.msgbody_len());
}

TEST(ProtoMsg, MsgBodySetGet)
{
    MsgBody body;
    body.set_sbody("test payload");
    EXPECT_EQ("test payload", body.sbody());
}

TEST(ProtoMsg, MsgBodyEmpty)
{
    MsgBody body;
    EXPECT_TRUE(body.sbody().empty());
}

TEST(ProtoMsg, HttpMsgType)
{
    HttpMsg msg;
    msg.set_type(1);  // HTTP_REQUEST = 1
    EXPECT_EQ(1, msg.type());
}

TEST(ProtoMsg, SerializeParseRoundTrip)
{
    MsgHead head;
    head.set_cmd(100);
    head.set_seq(200);
    head.set_msgbody_len(0);  // required field

    MsgBody body;
    body.set_sbody("round_trip_data");

    std::string headBytes = head.SerializeAsString();
    std::string bodyBytes = body.SerializeAsString();

    MsgHead parsedHead;
    EXPECT_TRUE(parsedHead.ParseFromString(headBytes));
    EXPECT_EQ(100u, parsedHead.cmd());
    EXPECT_EQ(200u, parsedHead.seq());

    MsgBody parsedBody;
    EXPECT_TRUE(parsedBody.ParseFromString(bodyBytes));
    EXPECT_EQ("round_trip_data", parsedBody.sbody());
}

TEST(ProtoMsg, LargeBodySerializeParse)
{
    std::string large(100000, 'Z');
    MsgBody body;
    body.set_sbody(large);

    std::string bytes = body.SerializeAsString();
    MsgBody parsed;
    EXPECT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(large, parsed.sbody());
}

// Raft 相关消息 (namespace coor)
TEST(ProtoCoor, RaftRequestVoteDefaults)
{
    coor::RaftRequestVote req;
    EXPECT_EQ(0u, req.term());
    EXPECT_TRUE(req.candidate_id().empty());
    EXPECT_EQ(0u, req.last_log_index());
    EXPECT_EQ(0u, req.last_log_term());
}

TEST(ProtoCoor, RaftRequestVoteSetGet)
{
    coor::RaftRequestVote req;
    req.set_term(5);
    req.set_candidate_id("candidate_3");
    req.set_last_log_index(10);
    req.set_last_log_term(4);
    req.set_next_node_id_alloc_hint(42);

    EXPECT_EQ(5u, req.term());
    EXPECT_EQ("candidate_3", req.candidate_id());
    EXPECT_EQ(10u, req.last_log_index());
    EXPECT_EQ(4u, req.last_log_term());
    EXPECT_EQ(42u, req.next_node_id_alloc_hint());
}

TEST(ProtoCoor, RaftRequestVoteRspDefaults)
{
    coor::RaftRequestVoteRsp rsp;
    EXPECT_EQ(0u, rsp.term());
    EXPECT_FALSE(rsp.vote_granted());
}

TEST(ProtoCoor, RaftRequestVoteRspSetGet)
{
    coor::RaftRequestVoteRsp rsp;
    rsp.set_term(7);
    rsp.set_vote_granted(true);
    rsp.set_voter_next_node_id_alloc_hint(99);

    EXPECT_EQ(7u, rsp.term());
    EXPECT_TRUE(rsp.vote_granted());
    EXPECT_EQ(99u, rsp.voter_next_node_id_alloc_hint());
}

TEST(ProtoCoor, RaftAppendEntriesDefaults)
{
    coor::RaftAppendEntries req;
    EXPECT_EQ(0u, req.term());
    EXPECT_EQ(0u, req.prev_log_index());
}

TEST(ProtoCoor, RaftAppendEntriesSetGet)
{
    coor::RaftAppendEntries req;
    req.set_term(3);
    req.set_leader_id("leader_2");
    req.set_prev_log_index(100);
    req.set_prev_log_term(2);
    req.set_leader_commit(99);
    req.set_online_nodes_seq(1);

    EXPECT_EQ(3u, req.term());
    EXPECT_EQ("leader_2", req.leader_id());
    EXPECT_EQ(100u, req.prev_log_index());
    EXPECT_EQ(2u, req.prev_log_term());
    EXPECT_EQ(99u, req.leader_commit());
    EXPECT_EQ(1u, req.online_nodes_seq());
}

TEST(ProtoCoor, RaftAppendEntriesRspDefaults)
{
    coor::RaftAppendEntriesRsp rsp;
    EXPECT_EQ(0u, rsp.term());
    EXPECT_FALSE(rsp.success());
}

TEST(ProtoCoor, RaftRequestVoteSerializeParse)
{
    coor::RaftRequestVote req;
    req.set_term(10);
    req.set_candidate_id("cand_5");
    req.set_next_node_id_alloc_hint(200);

    std::string bytes = req.SerializeAsString();
    coor::RaftRequestVote parsed;
    EXPECT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(10u, parsed.term());
    EXPECT_EQ("cand_5", parsed.candidate_id());
    EXPECT_EQ(200u, parsed.next_node_id_alloc_hint());
}

TEST(ProtoCoor, NodeReportDefaults)
{
    NodeReport report;
    EXPECT_TRUE(report.node_type().empty());
    EXPECT_EQ(0u, report.node_id());
}

TEST(ProtoCoor, NodeReportSetGet)
{
    NodeReport report;
    report.set_node_type("logic");
    report.set_node_id(42);
    report.set_node_ip("10.0.0.1");

    EXPECT_EQ("logic", report.node_type());
    EXPECT_EQ(42u, report.node_id());
    EXPECT_EQ("10.0.0.1", report.node_ip());
}
