#!/usr/bin/env python3
"""查询 etcd 在线节点列表。用法: python3 admin_nodes.py [--endpoint http://127.0.0.1:2379]"""
import argparse, base64, json, sys, urllib.request

def b64(s): return base64.b64encode(s.encode()).decode()
def b64dec(s): return base64.b64decode(s).decode()

def etcd_get(endpoint, key, prefix=False):
    url = f"{endpoint}/v3/kv/range"
    body = {"key": b64(key)}
    if prefix:
        r = key[:-1] + chr(ord(key[-1]) + 1)
        body["range_end"] = b64(r)
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    resp = urllib.request.urlopen(req, timeout=3)
    kvs = json.loads(resp.read()).get("kvs", [])
    return [{"key": b64dec(k["key"]), "value": b64dec(k["value"]), "lease": k.get("lease","-")} for k in kvs]

def main():
    p = argparse.ArgumentParser(description="查看 etcd 在线节点")
    p.add_argument("--endpoint", default="http://127.0.0.1:2379")
    args = p.parse_args()
    try:
        entries = etcd_get(args.endpoint, "/thunder/registry/", prefix=True)
    except Exception as e:
        print(f"连接 etcd 失败: {e}", file=sys.stderr); sys.exit(1)
    print(f"{'node_id':>8}  {'node_type':<12}  {'ip:port':<22}  lease")
    print("-" * 65)
    for e in entries:
        ip = e["key"].replace("/thunder/registry/", "")
        try:
            v = json.loads(e["value"]); nid = v.get("node_id","?"); nt = v.get("node_type","?")
        except: nid = e["value"]; nt = "?"
        print(f"{str(nid):>8}  {nt:<12}  {ip:<22}  {e['lease']}")
    print(f"\n共 {len(entries)} 个在线节点")
if __name__ == "__main__": main()
