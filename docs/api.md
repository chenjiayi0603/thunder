# Thunder HTTP API

## HelloHttp (port 27006)

### POST /hello/hello
业务请求，返回 JSON。

```bash
curl -X POST http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# {"code":0,"msg":"ok","size":50,"data":"..."}
```

### POST /hello/raw
Fast-Path 直通，绕过 Protobuf 编解码。

```bash
curl -X POST http://127.0.0.1:27006/hello/raw -d '{}'
# {"code":0,"msg":"ok"}
```

### POST /hello/lua_echo
Lua 脚本处理（热更新支持）。

```bash
curl -X POST http://127.0.0.1:27006/hello/lua_echo -d 'test'
# {"code":0}
```

### GET /health
健康检查。

```bash
curl http://127.0.0.1:27006/health
# {"status":"ok"}
```

## Interface (port 27008)

### POST /Interface/gentoken
GenKey/VerifyKey 演示。

```bash
curl -X POST http://127.0.0.1:27008/Interface/gentoken -d '{"option":"GenKey"}'
# {"code":0,"msg":"success","token":"...","key":"..."}
```

## HelloWs (port 27010)

WebSocket 端点: `ws://127.0.0.1:27010/hello/shake`

## HelloHttps (port 27443)

HTTPS 端点，同 HelloHttp 路径。
