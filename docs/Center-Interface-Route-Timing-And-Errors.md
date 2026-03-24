# Center → Interface 路由时序与常见日志

## 概括：选主 / 下发路由 / 间隔

```
选主（代码侧，多 Center）
  SessionRaftCluster 定时器周期 1s（CenterCmd.json 中该 session 超时）
  冷启动首次允许拉选：m_raftFollowerDeadline ≈ 0.20～0.50s（kFollowerColdStart*）+ 按拍执行 → 常见约 1～数秒级出 Leader（视机器与 RPC）
  跟主后误判掉主：租约 ≈ 2*center_beat + 1.0～1.5s（默认 center_beat=3 时约 7～7.5s 无 AE 才抢选）
  centers 为空单节点：无选举，启动即 Leader

选主（脚本侧，保守等待）
  CENTER_RAFT_SETTLE_SEC / RAFT_SETTLE_SEC 默认 5s（三实例起完后 sleep）

下发路由（Center → Interface）
  Leader 上 Logic 注册成功 → AddNodeBroadcast → cmd15 异步发送：正常多为毫秒～1～2s 内到 Interface
  Notice 队列重试：每 1s 扫，距入队 ≥2s 可再发
  心跳补发同节点路由：Leader 上间隔 ≥8s 才可能再广播（kMinHeartbeatRouteBroadcastSec）
  Leader 对 Follower 心跳：center_beat 默认 2s（CenterCmd.json）

联调脚本串行间隔（默认，三 Center）
  5s(Raft) + 8s(Logic) + 4s(Interface) ≈ 17s 后才 curl；单 Center 约 4+2=6s

协程 Step（参考）
  Step 单次 lifetime 常 5s；累计超时次数满则 OnCoroutineError（与路由是否已到无关）
```

图下：脚本间隔是人为保底；代码里选主、Notice、心跳是另一套计时，二者勿混。

---

## 路由怎么到 Interface

```
Logic --NodeRegister--> Center(须是 Raft Leader)
                              |
                              v
                    AddNodeBroadcast / SendNodeNotice
                              |
                              v
              CMD_REQ_NODE_REG_NOTICE (cmd 15)
                              |
                              v
Interface Worker ------> CmdNodeNotice --> AddNodeIdentify(LOGIC)
```

图下：Follower 不收新注册；Leader 才入表并广播。CenterCmd 里 INTERFACE 须 subscribe LOGIC。

---

## 多 Center 时序（示意）

```
  [起 N 个 Center] --> [Raft 选主] --> [Logic 连上 Leader 注册]
         |                                      |
         | CENTER_RAFT_SETTLE_SEC               | LOGIC_REGISTER_WAIT_SEC
         v                                      v
  [起 Interface] --> [再等一会] --> [curl GenKey]
         |
         INTERFACE_ROUTE_READY_SEC
```

图下：HTTP 若早于右侧竖线完成，会无 LOGIC 路由。

---

## 脚本默认秒数（三实例 / 单实例）

```
三 Center:  RAFT settle 5s  ->  Logic wait 8s  ->  Interface wait 4s  ->  curl
单 Center:  (无 RAFT)       ->  Logic wait 4s  ->  STARTUP_WAIT_SEC 2s ->  curl
```

变量名：`CENTER_RAFT_SETTLE_SEC`，`LOGIC_REGISTER_WAIT_SEC`，`INTERFACE_ROUTE_READY_SEC`，`test_multicenter_raft.sh` 里 `RAFT_SETTLE_SEC`。

---

## 日志里两种「错」

### A) no tagMsgShell match LOGIC

```
GenKey --> SendToSession(LOGIC) --> NodesMgr 无 LOGIC --> 失败
                |
                +--> ResponseToClient --> SendTo(fd7) 回 JSON 错误（不是发往 LOGIC）
```

图下：路由未到或请求太早。fd7 是 HTTP 客户端。

### B) no fd7 / operation timeout

```
无路由分支同步跑完 --> 已回 HTTP --> 客户端关连接 --> fd7 无效
        |
        +--> [旧 bug] Emit 仍返回 RUNNING --> Step 继续超时
                    |
                    v
            OnCoroutineError --> 再 SendTo(fd7) --> no fd7
```

图下：与当时是否已有 LOGIC 路由无关。修复：`StepCo20::Emit` 同步结束时返回 COMPLETED，删掉 Step。

---

## 一条请求与时间（对照日志）

```
t0    Interface 起
t1    GenKey（无路由）-> no LOGIC
t2    Notice 到 -> AddNodeIdentify(LOGIC)  [t2 > t1]
t3    Step 超时再写 fd7 -> no fd7         [若未修 Emit]
```

图下：t1 无路由、t2 有路由可同时成立；t3 是死连接上再发，不是「又没路由」。

---

## 代码与脚本索引

| 项 | 路径 |
|----|------|
| 路由/广播/心跳补发 | `code/Center/src/SessionOnlineNodes.cpp` |
| Raft | `code/Center/src/SessionRaftCluster.cpp` |
| 协程 Step 结束 | `code/Net/src/step/StepCo20.cpp` `Emit()` |
| 按类型发送 | `code/Net/src/labor/Worker.cpp` `SendToNext` |
| 联调 | `deploy/tests/test_interfaceserver.sh` |
| 配置 | `deploy/Center/conf/CenterCmd.json` |

以当前仓库代码为准。
