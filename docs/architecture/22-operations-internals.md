# 运维内幕 — 连接管理 · 插件 · 性能优化

> 源码: `code/Net/src/labor/Worker.cpp`

---

## 连接管理

### 连接属性

```cpp
struct tagConnectionAttr {
    std::unique_ptr<CBuffer> pRecvBuff;
    std::unique_ptr<CBuffer> pSendBuff;
    std::unique_ptr<CBuffer> pClientData;
    char   szRemoteAddr[32];
    E_CODEC_TYPE eCodecType;
    int    iFd;
    uint32 ulSeq;              // FD 序列号（防 ABA）
    std::string strIdentify;   // 连接标识 (e.g. "logic:192.168.1.1:8080.0")
    ev_io* pIoWatcher;
    ev_timer* pTimeWatcher;
};
```

### ABA 防护（FD 序列号）

```
问题: fd 被 close 后，新 accept 可能复用同一 fd 值
      旧的 ev_io watcher 回调会误操作新连接

方案: 每个 fd 在创建时分配单调递增的 ulSeq
      IoCallback 中验证 pData->ulSeq == pConn->ulSeq
      不匹配则 DelEvent（丢弃过期回调）

IoCallback:
  if (pData->ulSeq != pConn->ulSeq):
    DelEvent(watcher, pData)
    return
  IoRead(...) / IoWrite(...):
    auto iter = mapFdAttr.find(iFd);
    if (iter == mapFdAttr.end() || iter->second->ulSeq != ulSeq):
      return  ← 连接已被销毁
```

### S2S 连接建立

```
Node-A Manager                Node-B Manager
      │                              │
      │ connect() ─────────────────► │
      │                              │ accept() → CreateAcceptFdAttr
      │◄────── connected ─────────── │
      │                              │
      │ CMD_REQ_CONNECT_TO_WORKER    │
      │   {worker_idx} ────────────► │
      │                              │ send_fd_to_worker(clientFd)
      │                              │
      │ CMD_REQ_TELL_WORKER ◄─────── │
      │   {node_type, identify}      │
      │                              │
      │◄══════ normal traffic ══════► │
```

### 发送路由策略

```
SendTo(identify)        → 精确路由到指定 identify
SendToNext(identify, cmd)→ 轮询同一 identify 的多个连接
SendToNextByMod(uid)    → 按 uid % worker_num 分发（一致性 hash 变体）
SendToNextByMinLoad()   → 选择负载最小的连接
```

---

## 插件系统

### 动态加载流程

```json
{
  "so": {"CmdLogic": {"path": "./libCmdLogic.so", "symbol": "CreateCmd"}},
  "module": {"ModuleAuth": {"path": "./libModuleAuth.so", "symbol": "CreateModule"}}
}
```

```
Worker::LoadSo(conf):
  for each so in conf:
    dlopen(path, RTLD_NOW)
    dlsym(handle, symbol) → CreateCmd function pointer
    Cmd* pCmd = CreateCmd()
    AddCmd(pCmd, cmd_id)

Worker::LoadModule(conf):
  for each module in conf:
    dlopen(path, RTLD_NOW)
    dlsym(handle, symbol) → CreateModule function pointer
    Module* pModule = CreateModule()
    RegisterModule(pModule, url_path)
```

---

## 性能优化要点

### Session 超时

```
Session::OnTimeout(SessionType, SessionId):
  auto iter = mapSession.find(key)
  if iter != mapSession.end() && iter->second.ulSessionId == ulSessionId:
    ReleaseSession(iter)
```

用 `ulSessionId` 防 ABA：session id 复用但 session 对象不同时，ID 单调递增确保不会误删。

### 消息路由开销

```
Dispose(msgHead, msgBody):
  if msgHead.cmd() == 0:
    return  // 无处理者
  auto iter = mapCmd.find(msgHead.cmd())
  if iter != mapCmd.end():
    iter->second->AnyMessage(shell, msgHead, msgBody)  // 直接 O(1) 查找
```

### Worker 负载上报

```
CMD_REQ_UPDATE_WORKER_LOAD → Manager
  内容: connect_count, recv_count, send_count, client_count
  Manager 用这些指标做跨 Worker 负载均衡
```
