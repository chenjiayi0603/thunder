#!/usr/bin/env python3
"""Thunder etcd 管理工具。用法: admin.py {nodes|routes|status|config} [args...]"""
import argparse, base64, json, os, sys, urllib.request, urllib.error

_DEFAULT_EP = "http://127.0.0.1:2379"


def _load_endpoints_from_config() -> list[str]:
    """Read etcd_endpoints from deploy/Logic/conf/Logic.json if available."""
    here = os.path.dirname(os.path.abspath(__file__))
    cfg = os.path.join(here, "..", "Logic", "conf", "Logic.json")
    try:
        d = json.load(open(cfg))
        eps = d.get("center", {}).get("etcd_endpoints", "") or d.get("etcd_endpoints", "")
        if eps:
            return [e.strip() for e in eps.split(",") if e.strip()]
    except Exception:
        pass
    return [_DEFAULT_EP]


def b64(s): return base64.b64encode(s.encode()).decode()
def b64dec(s): return base64.b64decode(s).decode()


def etcd_get(endpoint, path, body=None):
    data = json.dumps(body or {}).encode()
    req = urllib.request.Request(
        f"{endpoint}{path}", data=data,
        headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=3).read())


def etcd_range(endpoint, prefix):
    end = prefix[:-1] + chr(ord(prefix[-1]) + 1)
    return etcd_get(endpoint, "/v3/kv/range",
                    {"key": b64(prefix), "range_end": b64(end)})


def parse_nodes(data):
    nodes = []
    for kv in data.get("kvs", []):
        key = b64dec(kv["key"])
        # key format: /thunder/registry/{node_type}/{ip}:{port}
        parts = key.replace("/thunder/registry/", "", 1).split("/", 1)
        addr = parts[1] if len(parts) == 2 else key
        try:
            v = json.loads(b64dec(kv["value"]))
            nid = v.get("node_id", "?")
            nt = v.get("node_type", "?")
        except Exception:
            nid = "?"; nt = parts[0] if parts else "?"
        nodes.append({"node_id": nid, "type": nt, "addr": addr,
                      "lease": kv.get("lease", "0")})
    return nodes


def cmd_nodes(args):
    data = etcd_range(args.endpoints[0], "/thunder/registry/")
    nodes = parse_nodes(data)
    if not nodes:
        print("(无在线节点)")
        return
    print(f"{'node_id':>8}  {'node_type':<14}  {'ip:port':<22}  lease")
    print("-" * 72)
    for n in sorted(nodes, key=lambda x: str(x["node_id"])):
        print(f"{str(n['node_id']):>8}  {n['type']:<14}  {n['addr']:<22}  {n['lease']}")
    nids = [n["node_id"] for n in nodes]
    dup = len(nids) != len(set(nids))
    orphans = sum(1 for n in nodes if n["lease"] == "0")
    print(f"\n共 {len(nodes)} 个在线节点", end="")
    if dup: print("  ⚠️  node_id 重复!", end="")
    if orphans: print(f"  ⚠️  {orphans} 个孤儿键(lease=0)", end="")
    print()


def cmd_routes(args):
    data = etcd_range(args.endpoints[0], "/thunder/registry/")
    nodes = parse_nodes(data)
    if not nodes:
        print("(无路由信息 — 无在线节点)")
        return
    routes: dict[str, list[str]] = {}
    for n in nodes:
        routes.setdefault(n["type"], []).append(f"{n['addr']} (node_id={n['node_id']})")
    print(f"{'node_type':<16} {'节点数':<8} 路由目标")
    print("-" * 65)
    for nt in sorted(routes):
        rs = routes[nt]
        print(f"{nt:<16} {len(rs):<8} {', '.join(rs)}")
    if "INTERFACE" in routes and "LOGIC" in routes:
        print(f"\n✅ Interface→Logic S2S 链路: "
              f"{len(routes['INTERFACE'])} Interface × {len(routes['LOGIC'])} Logic")


def _check_endpoint_health(ep: str) -> str:
    try:
        data = urllib.request.urlopen(f"{ep}/health", timeout=3).read()
        h = json.loads(data).get("health", "?")
        return "✅ healthy" if h == "true" else f"⚠️  {h}"
    except Exception as e:
        return f"❌ {e}"


def cmd_status(args):
    # Health for every configured endpoint
    print("=== 端点健康 ===")
    for ep in args.endpoints:
        status = _check_endpoint_health(ep)
        print(f"  {ep:<38}  {status}")

    # Cluster metadata from the first reachable endpoint
    print("\n=== 集群信息 ===")
    for ep in args.endpoints:
        try:
            r = etcd_get(ep, "/v3/maintenance/status", {})
            h = r.get("header", {})
            print(f"  revision    : {h.get('revision', '?')}")
            print(f"  raft_term   : {h.get('raft_term', '?')}")
            print(f"  version     : {r.get('version', '?')}")
            print(f"  db_size     : {r.get('dbSize', '?')} bytes")
            break
        except Exception:
            continue

    # Member list
    print("\n=== 成员列表 ===")
    for ep in args.endpoints:
        try:
            r = etcd_get(ep, "/v3/cluster/member/list", {})
            members = r.get("members", [])
            for m in members:
                urls = ", ".join(m.get("clientURLs", []))
                print(f"  {m.get('name', '?'):<10}  {urls}")
            break
        except Exception:
            continue


def cmd_config(args):
    P = "/thunder/config/"
    ep = args.endpoints[0]
    sub = args.config_cmd
    if sub == "list":
        r = etcd_range(ep, P)
        kvs = r.get("kvs", [])
        if not kvs:
            print("(无配置项)")
            return
        for kv in kvs:
            k = b64dec(kv["key"]).replace(P, "")
            v = b64dec(kv["value"])
            print(f"  {k} = {v[:200]}{'...' if len(v) > 200 else ''}")
    elif sub == "get":
        if not args.key:
            print("用法: admin.py config get <key>"); sys.exit(1)
        r = etcd_get(ep, "/v3/kv/range", {"key": b64(P + args.key)})
        kvs = r.get("kvs", [])
        if kvs:
            print(b64dec(kvs[0]["value"]))
        else:
            print(f"(key not found: {P}{args.key})")
    elif sub == "set":
        if not args.key or not args.value:
            print("用法: admin.py config set <key> <value>"); sys.exit(1)
        etcd_get(ep, "/v3/kv/put",
                 {"key": b64(P + args.key), "value": b64(args.value)})
        print(f"✅ {P}{args.key}")


def main():
    default_eps = _load_endpoints_from_config()

    p = argparse.ArgumentParser(description="Thunder etcd 管理工具")
    p.add_argument(
        "--endpoints", default=",".join(default_eps),
        help=f"逗号分隔的 etcd 端点列表 (默认读自 Logic.json)")
    sub = p.add_subparsers(dest="cmd")

    sub.add_parser("nodes",  help="查看在线节点").set_defaults(func=cmd_nodes)
    sub.add_parser("routes", help="查看路由表 (node_type→节点映射)").set_defaults(func=cmd_routes)
    sub.add_parser("status", help="etcd 集群健康 + 成员").set_defaults(func=cmd_status)

    cp = sub.add_parser("config", help="查改配置")
    cp.add_argument("config_cmd", choices=["list", "get", "set"])
    cp.add_argument("key",   nargs="?")
    cp.add_argument("value", nargs="?")
    cp.set_defaults(func=cmd_config)

    args = p.parse_args()
    args.endpoints = [e.strip() for e in args.endpoints.split(",") if e.strip()]

    if not hasattr(args, "func"):
        p.print_help(); sys.exit(1)

    try:
        args.func(args)
    except urllib.error.URLError as e:
        hint = "\n请先执行 ./deploy.sh up 启动集群" if "refused" in str(e).lower() else ""
        print(f"❌ etcd 不可达 ({args.endpoints[0]}): {e}{hint}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.HTTPError as e:
        print(f"❌ etcd HTTP {e.code} ({args.endpoints[0]})", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"❌ 错误: {e}", file=sys.stderr); sys.exit(1)


if __name__ == "__main__":
    main()
