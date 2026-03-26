
center 支持网页查看
net 支持 并行库（并行库考虑tbb、openmp、线程池）

对center 发送 需要只是发主；没主时才都发

center同步路由镜像的计划：在业务节点（manger进程）定时上报主中心时，主中心每隔一段时间回应消息里携带该节点关注的路由信息（例如interface关注logic，则发送给interface的是logic路由），业务节点（manger进程）收到后写入共享内存（只是保存路由的共享内存），然后由业务节点（worker进程）定时感知到后更新自己的路由。center同步配置的计划：主中心节点收到web管理功能 写入的某类型节点自定义配置后（对应服务器的配置里的"custom": {}），发送对应类型的业务节点（loader进程），收到后写入共享内存（只是保存自定义配置），然后由业务节点（worker进程）定时感知到后更新自己的自定义配置（对应服务器的配置里的"custom": {}）。


业务节点的manager 收到路由通知后写完共享内存（要是路由信息完全相同就不用写，要是写了就版本号递增一；共享内存包含路由信息、整个路由的版本号），只在nodeid更新才主动通知woker关于nodeid更新，不会主动冲通知woker关于路由的镜像，woker定时检查镜像版本，版本变化了才更新路由


还建议补 4 个小点，计划就更“可直接施工”：

判等标准固定
在 Manager 写路由 shm 前，明确用“序列化后二进制完全一致”还是“规范化后比较（排序/去抖）”。建议先用二进制一致，简单可靠。（使用二进制一致）

并发可见性约束写进计划
写入顺序固定为：blob -> len -> version++；读取按 version 变化触发，避免半包读取。（顺序为blob -> len -> version++）

容量与降级策略明确
路由 shm 建议直接定到 160KB；超限时不写入、打错误日志并保留旧版本（Worker继续用旧路由）。（路由 shm 160KB，配置 shm 160KB）

custom 同步链路的入口命令定名
你计划里已有方向，但最好先定一个明确命令/消息体（如 CMD_REQ_CUSTOM_CONFIG_SYNC + {node_type, custom, version}），这样开发和联调不会跑偏。

message NodeCustomConfig
{
	string custom_config = 1;
}

CMD_REQ_UPDATE_CONFIG_FILE			= 1,



dpdk的思考
