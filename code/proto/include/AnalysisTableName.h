/*******************************************************************************
* Project:  proto
* @file     RobotTableName.h
* @brief    IM数据库表名定义
* @author   JiangJianyu
* @date:    2016年04月32日
* @note
* Modify history:
******************************************************************************/
#ifndef SRC_ROBOTTABLENAME_H_
#define SRC_ROBOTTABLENAME_H_

namespace analysis
{
//db_analysis_event_conf
#define tb_event_conf_name "tb_event_conf"
#define tb_page_name_conf_name "tb_page_name_conf"
#define tb_event_funnel_conf_name "tb_event_funnel_conf"
#define tb_session_length_conf_name "tb_session_length_conf"
#define tb_session_event_num_conf_name "tb_session_event_num_conf"
#define tb_operators_conf_name "tb_operators_conf"

//db_analysis_event_statistics
#define tb_event_statistics_name "tb_event_statistics"
#define tb_funnel_statistics_name "tb_funnel_statistics"

//db_analysis_daily_statistics
#define tb_daily_statistics_name "tb_daily_statistics"
#define tb_realtime_statistics_name "tb_realtime_statistics"

#define tb_day_session_length_seg_name "tb_day_session_length_seg"
#define tb_day_session_event_num_seg_name "tb_day_session_event_num_seg"
#define tb_page_view_name "tb_page_view"
#define tb_page_route_name "tb_page_route"

#define db_analysis_platform_name "db_analysis_platform"
#define db_analysis_event_conf_name "db_analysis_event_conf"
#define db_analysis_event_statistics_name "db_analysis_event_statistics"
#define db_analysis_daily_statistics_name "db_analysis_daily_statistics"

#define tb_new_retained_user_name "tb_new_retained_user"
#define tb_active_retained_user_name "tb_active_retained_user"

#define db_analysis_retain_statistics_name "db_analysis_retain_statistics"

}

#endif /* SRC_ROBOTTABLENAME_H_ */
