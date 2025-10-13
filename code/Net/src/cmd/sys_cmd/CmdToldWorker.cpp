/*******************************************************************************
 * Project:  Net
 * @file     CmdToldWorker.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <cmd/sys_cmd/CmdToldWorker.hpp>

namespace net
{

bool CmdToldWorker::AnyMessage(const tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const MsgBody& oInMsgBody)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    TargetWorker oInTargetWorker;
    TargetWorker oOutTargetWorker;
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(oInMsgHead.seq());

	if (oInTargetWorker.ParseFromString(oInMsgBody.body()))
	{
		LOG4_TRACE("AddMsgShell(%s, fd %d, seq %llu)!",oInTargetWorker.worker_identify().c_str(), stMsgShell.iFd, stMsgShell.ulSeq);
		GetLabor()->AddMsgShell(oInTargetWorker.worker_identify(), stMsgShell);
		GetLabor()->AddNodeIdentify(oInTargetWorker.node_type(), oInTargetWorker.worker_identify());
		GetLabor()->AddInnerFd(stMsgShell);
		oOutTargetWorker.set_err_no(0);
		oOutTargetWorker.set_worker_identify(GetLabor()->GetWorkerIdentify());
		oOutTargetWorker.set_node_type(GetLabor()->GetNodeType());
		oOutTargetWorker.set_err_msg("OK");
	}
	else
	{
		oOutTargetWorker.set_err_no(ERR_PARASE_PROTOBUF);
		oOutTargetWorker.set_worker_identify("unknow");
		oOutTargetWorker.set_node_type(GetLabor()->GetNodeType());
		oOutTargetWorker.set_err_msg("WorkerLoad ParseFromString error!");
		LOG4_ERROR("error %d: WorkerLoad ParseFromString error!", ERR_PARASE_PROTOBUF);
	}

    oOutMsgBody.set_body(oOutTargetWorker.SerializeAsString());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
    return(true);
}

} /* namespace net */
