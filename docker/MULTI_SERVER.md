# Thunder 多服务器部署

## 架构

```
服务器1 (Core):                    服务器2 (Gateway):
  etcd :2379                        Interface :27008
  Logic :16068                      Hello :27006
  Redis :6379                       HelloWS :27010
  MySQL :3306                       HelloHttps :27443
```

## 部署

```bash
# 服务器1 — 先启 etcd/mysql/redis
export SERVER1_IP=10.0.0.1
sed -i "s/SERVER1_IP/$SERVER1_IP/" docker-compose-multi.yml
docker compose -f docker-compose-multi.yml up -d etcd redis mysql logic

# 服务器2 — 等服务器1就绪后启各网关
# 先修改各节点 conf/*.json 的 etcd_endpoints 指向 SERVER1_IP:2379
docker compose -f docker-compose-multi.yml up -d interface hello hello_ws hello_https
```

## 配置要点

1. **etcd_endpoints**: 所有节点指向 `http://SERVER1_IP:2379`
2. **监听地址**: 改为 `0.0.0.0` (非 127.0.0.1)
3. **S2S 路由**: etcd registry 自动分发, 节点注册后全局可见
