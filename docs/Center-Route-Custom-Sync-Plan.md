# Center 路由镜像与配置同步实施计划

## 目标

- 路由镜像：Center 主节点按间隔在 `NodeReportRsp` 中回传“该节点订阅所需路由快照”，业务节点 `Manager` 写入路由共享内存，`Worker` 周期感知并更新本地路由。
- 自定义配置：Center 主节点收到 Web 管理写入后，按节点类型下发 `custom` 配置给业务节点 `Loader/Manager`，写入 custom 共享内存，`Worker` 周期感知并更新 `m_oCustomConf`。
- 两条链路解耦：`RouteNoticeVersionData` 只承载路由；`CustomConfigVersionData` 只承载 custom，不复用全量配置 shm。

## 关键约束（已确认）

- 判等标准：`Manager` 写路由 shm 前，使用 `NodeNotice` 序列化后二进制一致做判等。
- 写入顺序：固定 `blob -> len -> version++`。
- 容量：路由 shm `160KB`，配置 shm `160KB`。
- 路由通知策略：`Manager` 不主动通知 `Worker` 路由镜像变化；`Worker` 仅定时检查版本并按需更新。
- NodeId 策略：仅 `node_id` 更新时主动通知 `Worker`（沿用现有 `CMD_REQ_REFRESH_NODE_ID` 触发）。
- 失败策略：
  - 路由解析失败不推进消费版本（ack 不更新，保留重试）。
  - 配置下发失败延迟重发，最多 3 次。

## 路由镜像同步方案

### 1. Center 回传订阅路由快照

- 复用当前 `NodeReportRsp.subscribed_route_snapshot` 链路。
- 仅主中心回传；按中心配置间隔节流。
- 快照内容仅包含该节点订阅需要的目标节点类型路由。

关键文件：

- `code/Center/src/SessionOnlineNodes.cpp`

### 2. Manager 判等后写入路由 shm

- 在 `Manager` 处理 `CMD_RSP_NODE_REGISTER/CMD_RSP_NODE_STATUS_REPORT` 时：
  - 取 `subscribed_route_snapshot`；
  - 与当前 shm 中路由做二进制判等；
  - 相同：跳过写入，不增版本；
  - 不同：按 `blob -> len -> version++` 写入，并记录新版本。
- 同步处理 `node_id`：仅 `node_id` 变化时走主动通知 `Worker`。

关键文件：

- `code/Net/src/labor/Manager.cpp`
- `code/Net/include/labor/Labor.hpp`
- `code/Net/include/labor/types/RouteNoticeVersionData.hpp`

### 3. Worker 仅轮询版本并消费 shm

- 在 `Worker::CheckShareMem()` 周期检查 `route_version`：
  - `route_version <= ack`：不处理；
  - `route_version > ack`：拉取并解析 `NodeNotice`，更新路由；
  - 成功后推进 `ack`；
  - 解析失败不推进 `ack`，下周期重试。
- 不再依赖 Manager 主动下发路由更新通知。

关键文件：

- `code/Net/src/labor/Worker.cpp`
- `code/Net/src/cmd/sys_cmd/CmdUpdateNodeId.cpp`（仅保留 node_id 主动刷新职责）

### 4. 容量与观测

- `node_notice_blob` 提升到 `160KB`。
- 写入/消费日志包含：`version`、`bytes`、`reg_size`、`exit_size`、`skip_same`、`parse_failed`。

## custom 配置同步方案

### 1. Center 接收 Web 写入并按节点类型下发

- 入口命令：`CMD_REQ_UPDATE_CONFIG_FILE`。
- 消息体：

```proto
message NodeCustomConfig
{
    string custom_config = 1;
}
```

- 下发目标：对应节点类型业务节点（优先 `Loader`，无 `Loader` 时 `Manager` 兜底）。

关键文件：

- `deploy/Center/conf/Center.json`
- `code/Center/src/CmdNodeReport/CmdNodeReport.cpp`（或等价控制命令入口）

### 2. Net 新增 custom 专用共享内存

- 新增 `CustomConfigVersionData`：
  - `seq_custom`
  - `custom_len`
  - `custom_blob[160KB]`
- 对外接口：
  - `SetCustomConfig(...)`
  - `GetCustomConfig(...)`
  - `IsCustomVersionChange()`
  - `UpdateCustomVersion()`

关键文件：

- `code/Net/include/labor/types/CustomConfigVersionData.hpp`（新增）
- `code/Net/include/labor/Labor.hpp`
- `code/Net/src/labor/Manager.cpp`

### 3. 下发失败重试策略（最多 3 次）

- Center 发送失败进入延迟队列；
- 固定延迟（如 2s，可配置）重试；
- 最多 3 次，成功即出队；
- 超过 3 次记录错误并告警。

## 验收标准

- 路由：
  - 路由未变化时不写 shm、不增版本；
  - 路由变化后全部 Worker 在 1~2 个周期内收敛；
  - 解析失败时 ack 不推进，恢复后可继续追平。
- custom：
  - 目标节点收到后写 shm，Worker 周期内更新 `m_oCustomConf`；
  - 下发失败具备最多 3 次重试；
  - 非目标节点不受影响。

## 实现记录（Worker 读取共享内存）

- 统一入口：`code/Net/src/labor/Worker.cpp` 的 `Worker::CheckShareMem()`
- 配置共享内存（全量配置）：
  - `GetLoaderConfigVersionData().IsConfigVersionChange()`
  - `GetLoaderConfigVersionData().GetServerConfigFile(configContent)`
  - 成功后 `UpdateLoaderConfigVersion()`
- 路由共享内存（路由镜像）：
  - `GetRouteNoticeVersionData().IsNodeNoticeVersionChange()`
  - `GetRouteNoticeVersionData().GetNodeNotice(oNodeNotice)`
  - 成功后 `UpdateNodeNoticeVersion()`，并按全量快照重建+剪枝路由
  - 失败不推进 ack（保留下一周期重试）
- 自定义配置共享内存（custom）：
  - `GetCustomConfigVersionData().IsCustomVersionChange()`
  - `GetCustomConfigVersionData().GetCustomConfig(customContent)`
  - 解析成功后 `SetCustomConf(oCustomJson)` + `UpdateCustomVersion()`
  - 解析失败不推进 ack（保留下一周期重试）
