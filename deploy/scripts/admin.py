#!/usr/bin/env python3
"""Thunder etcd 管理工具。用法: admin.py {nodes|routes|status|config} [args...]"""
import argparse, base64, json, sys, urllib.request, urllib.error

EP = "http://127.0.0.1:2379"

def b64(s): return base64.b64encode(s.encode()).decode()
def b64dec(s): return base64.b64decode(s).decode()

def etcd_range(endpoint, prefix):
    end = prefix[:-1] + chr(ord(prefix[-1]) + 1)
    body = json.dumps({"key": b64(prefix), "range_end": b64(end)}).encode()
    req = urllib.request.Request(f"{endpoint}/v3/kv/range", data=body,
                                  headers={"Content-Type": "application/json"})
    resp = urllib.request.urlopen(req, timeout=3)
    return json.loads(resp.read())

def etcd_post(endpoint, path, body):
    d = json.dumps(body).encode()
    req = urllib.request.Request(f"{endpoint}{path}", data=d,
                                  headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=3).read())

def parse_nodes(data):
    nodes = []
    for kv in data.get("kvs", []):
        key = b64dec(kv["key"])
        ip = key.replace("/thunder/registry/", "")
        try:
            v = json.loads(b64dec(kv["value"]))
            nid = v.get("node_id", "?"); nt = v.get("node_type", "?")
        except:
            nid = b64dec(kv.get("value", "")); nt = "?"
        nodes.append({"node_id": nid, "type": nt, "addr": ip, "lease": kv.get("lease", "0")})
    return nodes

def cmd_nodes(args):
    data = etcd_range(args.endpoint, "/thunder/registry/")
    nodes = parse_nodes(data)
    if not nodes:
        print("(无在线节点)")
        return
    print(f"{'node_id':>8}  {'node_type':<12}  {'ip:port':<22}  lease")
    print("-" * 65)
    for n in sorted(nodes, key=lambda x: str(x["node_id"])):
        print(f"{str(n['node_id']):>8}  {n['type']:<12}  {n['addr']:<22}  {n['lease']}")
    # 一致性检查
    nids = [n["node_id"] for n in nodes]
    dup = len(nids) != len(set(nids))
    orphans = sum(1 for n in nodes if n["lease"] == "0")
    print(f"\n共 {len(nodes)} 个在线节点", end="")
    if dup: print("  ⚠️ node_id 重复!", end="")
    if orphans: print(f"  ⚠️ {orphans} 个孤儿键(lease=0)!", end="")
    print()

def cmd_routes(args):
    data = etcd_range(args.endpoint, "/thunder/registry/")
    nodes = parse_nodes(data)
    if not nodes:
        print("(无路由信息 — 无在线节点)")
        return
    # 按 node_type 分组展示路由表
    routes = {}
    for n in nodes:
        routes.setdefault(n["type"], []).append(f"{n['addr']} (node_id={n['node_id']})")
    print(f"{'node_type':<16} {'节点数':<8} 路由目标")
    print("-" * 65)
    for nt in sorted(routes):
        rs = routes[nt]
        print(f"{nt:<16} {len(rs):<8} {', '.join(rs)}")
    # 交叉验证 interface→logic 链路
    if "INTERFACE" in routes and "LOGIC" in routes:
        print(f"\n✅ Interface→Logic S2S 链路: {len(routes['INTERFACE'])} Interface × {len(routes['LOGIC'])} Logic")

def cmd_status(args):
    try:
        data = urllib.request.urlopen(f"{args.endpoint}/health", timeout=3).read()
        print(f"health: {data.decode().strip()}")
    except Exception as e:
        print(f"health: ❌ {e}")

    try:
        r = etcd_post(args.endpoint, "/v3/maintenance/status", {})
        h = r.get("header", {})
        print(f"revision: {h.get('revision','?')}  raft_term: {h.get('raft_term','?')}")
        print(f"version:  {r.get('version','?')}  dbSize: {r.get('dbSize','?')}")
    except: pass

    try:
        r = etcd_post(args.endpoint, "/v3/cluster/member/list", {})
        members = r.get("members", [])
        print(f"members:  {len(members)}")
        for m in members:
            print(f"  {m.get('name','?')}  {', '.join(m.get('clientURLs',[]))}")
    except: pass

def cmd_config(args):
    P = "/thunder/config/"
    sub = args.config_cmd
    if sub == "list":
        r = etcd_range(args.endpoint, P)
        kvs = r.get("kvs", [])
        if not kvs:
            print("(无配置项)")
            return
        for kv in kvs:
            k = b64dec(kv["key"]).replace(P, "")
            v = b64dec(kv["value"])
            print(f"  {k} = {v[:200]}{'...' if len(v)>200 else ''}")
    elif sub == "get":
        if not args.key: print("用法: admin.py config get <key>"); sys.exit(1)
        r = etcd_post(args.endpoint, "/v3/kv/range", {"key": b64(P + args.key)})
        kvs = r.get("kvs", [])
        if kvs: print(b64dec(kvs[0]["value"]))
        else: print(f"(key not found: {P}{args.key})")
    elif sub == "set":
        if not args.key or not args.value: print("用法: admin.py config set <key> <value>"); sys.exit(1)
        etcd_post(args.endpoint, "/v3/kv/put", {"key": b64(P + args.key), "value": b64(args.value)})
        print(f"✅ {P}{args.key}")

def main():
    p = argparse.ArgumentParser(description="Thunder etcd 管理工具")
    p.add_argument("--endpoint", default=EP, help=f"etcd 端点 (默认 {EP})")
    sub = p.add_subparsers(dest="cmd")

    sub.add_parser("nodes", help="查看在线节点").set_defaults(func=cmd_nodes)
    sub.add_parser("routes", help="查看路由表 (node_type→节点映射)").set_defaults(func=cmd_routes)
    sub.add_parser("status", help="etcd 集群健康 + 成员").set_defaults(func=cmd_status)

    cp = sub.add_parser("config", help="查改配置")
    cp.add_argument("config_cmd", choices=["list", "get", "set"])
    cp.add_argument("key", nargs="?")
    cp.add_argument("value", nargs="?")
    cp.set_defaults(func=cmd_config)

    args = p.parse_args()
    if not hasattr(args, 'func'):
        p.print_help(); sys.exit(1)
    try:
        args.func(args)
    except urllib.error.URLError as e:
        hint = "\n请先执行 ./deploy.sh up 启动集群" if "refused" in str(e).lower() else ""
        print(f"❌ etcd 不可达 ({args.endpoint}): {e}{hint}", file=sys.stderr); sys.exit(1)
    except urllib.error.HTTPError as e:
        hint = "\n请先执行 ./deploy.sh up 启动集群" if e.code == 502 else ""
        print(f"❌ etcd HTTP {e.code} ({args.endpoint}){hint}", file=sys.stderr); sys.exit(1)
    except Exception as e:
        print(f"❌ 错误: {e}", file=sys.stderr); sys.exit(1)

if __name__ == "__main__": main()
