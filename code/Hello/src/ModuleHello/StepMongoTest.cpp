/*******************************************************************************
 * Project:  Access
 * @file     StepMongoTest.cpp
 * @brief    访问多次mongo存储
 * @author   Tommy
 * @date:    2020年1月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "RobotRedisProto.h"
#include "StepMongoTest.hpp"
#include "user.pb.h"
#include "common.pb.h"
#include "util/CBuffer.hpp"

namespace im
{

StepMongoTest::StepMongoTest(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
:net::HttpStep(stMsgShell,oInHttpMsg)
{
	m_jsonObj.Parse(oInHttpMsg.body());
	if (m_jsonObj("val") == "update")
	{
		m_istage = eStage_update_mongo;
	}
	else if (m_jsonObj("val") == "pineline")
	{
		m_istage = eStage_pineline_mongo;
	}
}

net::E_CMD_STATUS StepMongoTest::Emit(int iErrno, const std::string& strErrMsg,const std::string& strErrShow)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	if (eStage_insert3_mongo == m_istage)
	{
		return Emit_findAndModify_mongo();//Emit_insert3_mongo();//
	}
	else if(eStage_insert_mongo == m_istage)
	{
		return Emit_insert_mongo();
	}
	else if(eStage_insert2_mongo == m_istage)
	{
		return Emit_insert2_mongo();
	}
	else if(eStage_upsert_mongo == m_istage)
	{
		return Emit_upsert_mongo();
	}
	else if(eStage_search_mongo == m_istage)
	{
		return Emit_search_mongo();
	}
	else if(eStage_search2_mongo == m_istage)
	{
		return Emit_search2_mongo();
	}
	else if (eStage_end == m_istage)
	{
		util::CJsonObject oJsonObj;
		oJsonObj.Add("code", robot::ERR_OK);
		oJsonObj.Add("msg", "ok");
		oJsonObj.Add("data", m_strResponseData);
		oJsonObj.Add("data2", m_strResponse2Data);
		SendToClient(oJsonObj.ToString());
		LOG4_TRACE("%s() done m_istage(%d)", __FUNCTION__,m_istage);
		return(net::STATUS_CMD_COMPLETED);
	}
	else if (eStage_update_mongo == m_istage)
	{
		return Emit_update_mongo();
	}
	else if(eStage_pineline_mongo == m_istage)
	{
		return Emit_pineline_mongo();
	}
	else
	{
		util::CJsonObject oJsonObj;
		oJsonObj.Add("code", robot::ERR_OK);
		oJsonObj.Add("msg", "ok");
		SendToClient(oJsonObj.ToString());
		LOG4_TRACE("%s() done m_istage(%d)", __FUNCTION__,m_istage);
		return(net::STATUS_CMD_COMPLETED);
	}
}
//coll = mongoc_client_get_collection (client, "db", "coll");
//   for (i = 0; i < 5; i++) {
//      bson_t reply;
//      bson_t *insert_cmd = BCON_NEW ("insert",
//                                     "coll",
//                                     "documents",
//                                     "[",
//                                     "{",
//                                     "x",
//                                     BCON_INT64 (i),
//                                     "}",
//                                     "]");

//BCON_CODE(_val)
net::E_CMD_STATUS StepMongoTest::Emit_insert3_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	util::CBsonObject requestjObj;
	{
		/*
				db.user.insert({
					uid: db.tt.findAndModify({        update:{$inc:{'id':1}},        query:{'name':'user'},        upsert:true,				new:true    }).id,
					username: "dotcoo",
					password:"dotcoo",
					info:"http://www.dotcoo.com/"
				}
				)
		 * */
//		requestjObj.AddCode("uid","db.tt.findAndModify({update:{$inc:{\"id\":1}},query:{\"name\":\"user\"}, upsert:true,new:true}).id");//AddCode
		requestjObj.AddCode("uid","getNextSequence(\"userid1\")");

		requestjObj.Add("username","dotcoo");
		requestjObj.Add("password","dotcoo");
		requestjObj.Add("info","http://www.dotcoo.com/");
	}
	std::string tablename = "usertest";
	net::MongoOperator oDbOperator(0,tablename.c_str(),DataMem::MemOperate::MongoOperate::INSERT);
	oDbOperator.AddVal(requestjObj.GetBson());

	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}


//db.tt.findAndModify({update:{$inc:{'id':10000} ,$set:{'updatetime':1595929134  } }, query:{'session':'123','updatetime':{$gte:1595929124} }, upsert:false,new:true})
//db.tt.findAndModify({update:{$set:{'updatetime':1595929124 ,'id':10000 } }, query:{'session':'123'}, upsert:false,new:true})
//db.tt.findAndModify({update:{$inc:{'id':1}},query:{'name':'user'}, upsert:true,new:true}).id
//db.tt.findAndModify({update:{$inc:{'id':10000 } }, query:{'session':'123'}, upsert:true,new:true})
//db.tt.findAndModify({update:{$set:{'id':10000 } }, query:{'session':'123','id':{$lt: 10000}}, upsert:false,new:true})
//db.tt.update ({'session':'123','id':{$lt: 10000}}, {$set:{'id':10000}})
//db.tt.update ({'session':'123',"updatetime" : { $lt: new Date(new Date().setDate(new Date().getDate()-1))  }}, {$set:{updatetime:new Date()  }}   })
//db.tt.findAndModify({update:{$inc:{'id':10000} ,$set:{'updatetime':1595929123  } }, query:{'session':'123' }, upsert:true,new:true})
net::E_CMD_STATUS StepMongoTest::Emit_findAndModify_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	util::CBsonObject condObj;
	{
		condObj.Add("name", "user1");
	}
	util::CBsonObject requestjObj;
	{
		/*
		 db.col.update({'likes':10001},{$set:{'likes':10001}})
		 * */
		util::CBsonObject valObj;
		valObj.Add("id",1);
		requestjObj.Add("$inc",valObj.GetBson());
	}

	util::CJsonObject fieldsObj;
	{
		fieldsObj.Add("name", 1);
		fieldsObj.Add("id", 1);
		fieldsObj.Add("_id",0);
	}

	std::string tablename = "col";
	net::MongoOperator oDbOperator(0,tablename.c_str(),DataMem::MemOperate::MongoOperate::UPSERT);
	oDbOperator.AddCond(condObj.GetBson());
	oDbOperator.AddVal(requestjObj.GetBson());
	oDbOperator.AddFields(fieldsObj.ToString());

	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}
//err_msg: "ok!"
//totalcount: 1
//curcount: 1
//record_data {
//  field_info {
//    col_value: "{ \"name\" : \"user1\", \"id\" : 27 }"
//  }
//}
//from: 2

net::E_CMD_STATUS StepMongoTest::Emit_insert_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	util::CJsonObject requestjObj;
	{
		/*
		 db.tb_coordinate_group.update({"group_id":1},{"group_id":1,"coordinate":[100,200],update_time:"2017"},true)
		 { "_id" : { "$oid" : "54bf1036a310012522417554" }, "group_id" : 137, "coordinate" : [ 113.940885, 22.543072 ],
			 "update_time" : 11111111 }
		 * */
		requestjObj.AddEmptySubArray("coordinate");
		requestjObj["coordinate"].Add(100.1);
		requestjObj["coordinate"].Add(200.2);
		requestjObj.Add("group_id", m_uiGroupId);
		requestjObj.Add("update_time", (uint32)::time(nullptr));
	}
	net::MongoOperator oDbOperator(m_uiGroupId,tb_coordinate,DataMem::MemOperate::MongoOperate::INSERT);
	oDbOperator.AddVal(requestjObj.ToString());
	LOG4_TRACE("%s() Mongo_Insert %s",__FUNCTION__,requestjObj.ToString().c_str());

	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}


net::E_CMD_STATUS StepMongoTest::Emit_insert2_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	util::CBsonObject oBsonObject;
	{
		oBsonObject.Add ( "0", "foo");
		oBsonObject.Add ("1", "bar");

		oBsonObject.Add( "id", 1);
		oBsonObject.Add("field1", 0);
		std::string msg = "test message";
		oBsonObject.Add( "bin1", (const uint8_t*)(msg.c_str()), (uint32_t)(msg.size()));
		const uint8_t data[] = {1, 2, 3, 4};
		oBsonObject.Add ("bin2", data, sizeof(data));
	}
	net::MongoOperator oDbOperator(m_uiGroupId,tb_coordinate2,DataMem::MemOperate::MongoOperate::INSERT);
	oDbOperator.AddVal(oBsonObject.GetBson());
	util::CJsonObject requestjObj;
	oBsonObject.ToJson(requestjObj);
	LOG4_TRACE("%s() Mongo_Insert %s",__FUNCTION__,requestjObj.ToString().c_str());

	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepMongoTest::Emit_upsert_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;

	util::CJsonObject condObj;
	{
		condObj.Add("group_id", m_uiGroupId);
	}
	util::CJsonObject requestjObj;
	{
		/*
		 db.tb_coordinate_group.update({"group_id":1},{"group_id":1,"coordinate":[100,200],update_time:"2017"},true)
		 { "_id" : { "$oid" : "54bf1036a310012522417554" }, "group_id" : 137, "coordinate" : [ 113.940885, 22.543072 ],
			 "update_time" : 11111111 }
		 * */
		requestjObj.AddEmptySubArray("coordinate");
		requestjObj["coordinate"].Add(100.1);
		requestjObj["coordinate"].Add(200.2);
		requestjObj.Add("group_id", m_uiGroupId);
		requestjObj.Add("update_time", (uint32)::time(nullptr));
	}
	net::MongoOperator oDbOperator(m_uiGroupId,tb_coordinate,DataMem::MemOperate::MongoOperate::UPSERT);
	oDbOperator.AddCond(condObj.ToString());
	oDbOperator.AddVal(requestjObj.ToString());

	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}


net::E_CMD_STATUS StepMongoTest::Emit_search_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	util::CJsonObject condObj;
	{
		condObj.Add("group_id", 1000);// $in/$nin/$or/$not
	}
	net::MongoOperator oDbOperator(m_uiGroupId,tb_coordinate,DataMem::MemOperate::MongoOperate::SELECT);
	oDbOperator.AddCond(condObj.ToString());

	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepMongoTest::Emit_search2_mongo()
{
	LOG4_TRACE("%s() Emit_search2_mongo", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	util::CJsonObject condObj;
	{
		condObj.Add("group_id", 1000);
	}
	util::CJsonObject fieldsObj;
	{
		fieldsObj.Add("group_id", 1);
		fieldsObj.Add("update_time", 1);
		fieldsObj.Add("_id",0);
	}
	util::CJsonObject sortObj;
	{
		sortObj.Add("update_time", -1);
	}
	net::MongoOperator oDbOperator(m_uiGroupId,tb_coordinate,DataMem::MemOperate::MongoOperate::SELECT);
	oDbOperator.AddCond(condObj.ToString());
	oDbOperator.AddFields(fieldsObj.ToString());
	oDbOperator.AddSort(sortObj.ToString());
	oDbOperator.AddLimit(10,3,200);
	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	LOG4_TRACE("oDbOperator(%s)!",oDbOperator.MakeMemOperate()->DebugString().c_str());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}
//db.col.update({'title':'MongoDB 教程'},{$set:{'title':'MongoDB'}},{multi:true})
/*
 (1)更新条件和更新数据,如db.a.update({"_id" : ObjectId("55ef549236fe322f9490e17b")},{"$set":{"key":"new_value","updated":true}})
 bson_t *cond = BCON_NEW ("_id", BCON_OID (&oid));//条件为id "_id" : ObjectId("55ef549236fe322f9490e17b")
 bson_t *updatedoc = BCON_NEW ("$set", "{",
 "key", BCON_UTF8 ("new_value"),
 "updated", BCON_BOOL (true),
 "}");//{"$set"}
 (2)更新文档
 UpdateDoc("mydb","mycoll",cond,updatedoc)
 (3)销毁请求
 if (query)
 bson_destroy (query);
 if (update)
 bson_destroy (update);
 * */
net::E_CMD_STATUS StepMongoTest::Emit_update_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	util::CBsonObject condObj;
	{
		condObj.Add("likes", 10001);
	}
	util::CBsonObject requestjObj;
	{
		/*
		 db.col.update({'likes':10001},{$set:{'likes':10001}})
		 * */
		util::CBsonObject valObj;
		valObj.Add("likes",10002);
		requestjObj.Add("$set",valObj.GetBson());
	}
	std::string tablename = "col";
	net::MongoOperator oDbOperator(0,tablename.c_str(),DataMem::MemOperate::MongoOperate::UPDATE);
	oDbOperator.AddCond(condObj.GetBson());
	oDbOperator.AddVal(requestjObj.GetBson());

	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepMongoTest::Emit_pineline_mongo()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	std::string tablename = "pineline";
	net::MongoOperator oDbOperator(0,tablename.c_str(),DataMem::MemOperate::MongoOperate::INSERT);
	for(int i = 0;i < 3;++i)
	{
		/*
		 db.pineline.insert({"id":1,"coordinate":[100,200],update_time:"2017"})
		 * */
		util::CJsonObject requestjObj;
		requestjObj.AddEmptySubArray("coordinate");
		requestjObj["coordinate"].Add(1000.1 + i);
		requestjObj["coordinate"].Add(2000.2 + i);
		requestjObj.Add("id", m_uiGroupId);
		requestjObj.Add("update_time", (uint32)::time(nullptr));
		oDbOperator.AddPinelineCmd(requestjObj.ToString());
	}
	oMsgBody.set_body(oDbOperator.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("MONGOAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to dataproxy error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}

void StepMongoTest::Callback_insert_mongo(const DataMem::MemRsp &oRsp)
{
	LOG4_TRACE("eStage_insert_mongo ok %s",oRsp.DebugString().c_str());
	std::string strData;
	if (oRsp.record_data_size() && oRsp.record_data(0).field_info_size())
	{
		for(int i = 0; i < oRsp.record_data_size();++i)
		{
			for(int j = 0; j < oRsp.record_data(0).field_info_size();++j)
			{
				strData += std::to_string(i) + "-" + std::to_string(j), oRsp.record_data(i).field_info(j).col_value();
			}
		}
	}
	LOG4_TRACE("strData %s",strData.c_str());
	//切换下一个状态
	m_istage = eStage_insert2_mongo;
}
void StepMongoTest::Callback_insert2_mongo(const DataMem::MemRsp &oRsp)
{
	LOG4_TRACE("eStage_insert2_mongo ok %s",oRsp.DebugString().c_str());
	std::string strData;
	if (oRsp.record_data_size() && oRsp.record_data(0).field_info_size())
	{
		for(int i = 0; i < oRsp.record_data_size();++i)
		{
			for(int j = 0; j < oRsp.record_data(0).field_info_size();++j)
			{
				strData += std::to_string(i) + "-" + std::to_string(j), oRsp.record_data(i).field_info(j).col_value();
			}
		}
	}
	LOG4_TRACE("strData %s",strData.c_str());
	//切换下一个状态
	m_istage = eStage_upsert_mongo;
}
void StepMongoTest::Callback_upsert_mongo(const DataMem::MemRsp &oRsp)
{
	LOG4_TRACE("eStage_upsert_mongo ok %s",oRsp.DebugString().c_str());
	std::string strData;
	if (oRsp.record_data_size() && oRsp.record_data(0).field_info_size())
	{
		for(int i = 0; i < oRsp.record_data_size();++i)
		{
			for(int j = 0; j < oRsp.record_data(0).field_info_size();++j)
			{
				strData += std::to_string(i) + "-" + std::to_string(j), oRsp.record_data(i).field_info(j).col_value();
			}
		}
	}
	LOG4_TRACE("strData %s",strData.c_str());
	//切换下一个状态
	m_istage = eStage_search_mongo;
}
void StepMongoTest::Callback_search_mongo(const DataMem::MemRsp &oRsp)
{
	LOG4_TRACE("eStage_search_mongo ok %s",oRsp.DebugString().c_str());
	util::CJsonObject objJson;
	util::CJsonObject objJsonTmp;
	if (oRsp.record_data_size())
	{
		for(int i = 0; i < oRsp.record_data_size();++i)
		{
			if(oRsp.record_data(i).field_info_size() > 0)
			{
				if (objJsonTmp.Parse(oRsp.record_data(i).field_info(0).col_value()))
				{
					objJson.Add(objJsonTmp);
				}
			}
		}
	}
	m_strResponseData = objJson;
	LOG4_TRACE("m_strResponseData %s",m_strResponseData.ToString().c_str());
	//切换下一个状态
	m_istage = eStage_search2_mongo;
}

/*
 eStage_search2_mongo ok err_no: 0
err_msg: "ok!"
totalcount: 10
curcount: 10
record_data {
  field_info {
    col_value: "{\"group_id\":1000,\"update_time\":1578490568}"
  }
}
record_data {
  field_info {
    col_value: "{\"group_id\":1000,\"update_time\":1578490585}"
  }
}
。。。
from: 2
 * */
void StepMongoTest::Callback_search2_mongo(const DataMem::MemRsp &oRsp)
{
	LOG4_TRACE("eStage_search2_mongo ok %s",oRsp.DebugString().c_str());
	util::CBsonObject obj;
	util::CBsonObject objTmp;
	if (oRsp.record_data_size() > 0)
	{
		for(int i = 0; i < oRsp.record_data_size();++i)
		{
			if(oRsp.record_data(i).field_info_size() > 0)
			{
				if (objTmp.Parse(oRsp.record_data(i).field_info(0).col_value()))
				{
					obj.Add(std::to_string(i),objTmp);
				}
			}
		}
	}
	m_strResponse2Data = obj.ToString();
	LOG4_TRACE("Callback_search2_mongo m_strResponse2Data %s",m_strResponseData.ToString().c_str());
	//切换下一个状态
	m_istage = eStage_end;
}
void StepMongoTest::Callback_update_mongo(const DataMem::MemRsp &oRsp)
{
	LOG4_TRACE("eStage_update_mongo ok %s",oRsp.DebugString().c_str());
	util::CJsonObject objJson;
	util::CJsonObject objJsonTmp;
	if (oRsp.record_data_size())
	{
		for(int i = 0; i < oRsp.record_data_size();++i)
		{
			if(oRsp.record_data(i).field_info_size() > 0)
			{
				if (objJsonTmp.Parse(oRsp.record_data(i).field_info(0).col_value()))
				{
					objJson.Add(objJsonTmp);
				}
			}
		}
	}
	LOG4_TRACE("%s() %s", __FUNCTION__,objJson.ToString().c_str());
	//切换下一个状态
	m_istage = eStage_update_end;
}

void StepMongoTest::Callback_pineline_mongo(const DataMem::MemRsp &oRsp)
{
	LOG4_TRACE("Callback_pineline_mongo ok %s",oRsp.DebugString().c_str());
	//切换下一个状态
	m_istage = eStage_pineline_end;
}

net::E_CMD_STATUS StepMongoTest::Callback(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data)
{
    LOG4_TRACE("seq[%u] StepMongoTest::Callback ok!", oInMsgHead.seq());
    if(net::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())//系统错误（如没有该指令）
	{
    	LOG4_ERROR("system response error");
    	Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");
		return net::STATUS_CMD_FAULT;
	}
	DataMem::MemRsp oRsp;
	if(!oRsp.ParseFromString(oInMsgBody.body()))
	{
		LOG4_ERROR("parse protobuf data fault");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");
		return net::STATUS_CMD_FAULT;
	}
	//读存储出错
	if(0 != oRsp.err_no())
	{
		if(oRsp.has_err_msg())
		{
			LOG4_ERROR("Callback error %d: %s!m_istage(%d)",oRsp.err_no(),oRsp.err_msg().c_str(),m_istage);
		}
		else
		{
			LOG4_ERROR("Callback error %d!m_istage(%d)", oRsp.err_no(),m_istage);
		}
		Response(oRsp.err_no(),oRsp.err_msg());
		return net::STATUS_CMD_FAULT;
	}
	if (eStage_insert3_mongo == m_istage)
	{
		Callback_insert_mongo(oRsp);
		m_istage = eStage_end;
	}
	else if (eStage_insert_mongo == m_istage)
    {
    	Callback_insert_mongo(oRsp);
    }
    else if (eStage_insert2_mongo == m_istage)
    {
    	Callback_insert2_mongo(oRsp);
    }
    else if (eStage_upsert_mongo == m_istage)
    {
    	Callback_upsert_mongo(oRsp);
    }
    else if (eStage_search_mongo == m_istage)
    {
    	Callback_search_mongo(oRsp);
    }
    else if (eStage_search2_mongo == m_istage)
	{
    	Callback_search2_mongo(oRsp);
	}
    else if (eStage_update_mongo == m_istage)
    {
    	Callback_update_mongo(oRsp);
    }
    else if (eStage_pineline_mongo == m_istage)
	{
    	Callback_pineline_mongo(oRsp);
	}
	LOG4_TRACE("%s()", __FUNCTION__);
	return(Emit(robot::ERR_OK));
}


void StepMongoTest::Response(int err_no,const std::string& msg)
{
	util::CJsonObject oJsonObj;
	oJsonObj.Add("code", err_no);
	oJsonObj.Add("msg", msg);
	SendToClient(oJsonObj.ToString());
}

net::E_CMD_STATUS StepMongoTest::Timeout()
{
	LOG4_WARN("%s()", __FUNCTION__);
	if (m_iTimeoutNum++ > 3)
	{
		LOG4_TRACE("%s()", __FUNCTION__);//超时3次放弃
		return(net::STATUS_CMD_FAULT);
	}
	return(Emit(robot::ERR_OK));//超时重试
}

} /* namespace im */
