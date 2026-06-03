#!/usr/bin/env python3
"""查改 etcd 配置。用法: python3 admin_config.py [--endpoint http://127.0.0.1:2379] {list|get <key>|set <key> <value>}"""
import argparse, base64, json, sys, urllib.request

def b64(s): return base64.b64encode(s.encode()).decode()
def b64dec(s): return base64.b64decode(s).decode()
P = "/thunder/config/"

def etcd_post(endpoint, path, body):
    d = json.dumps(body).encode()
    r = urllib.request.Request(f"{endpoint}{path}", data=d, headers={"Content-Type":"application/json"})
    return json.loads(urllib.request.urlopen(r, timeout=3).read())

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--endpoint", default="http://127.0.0.1:2379")
    p.add_argument("cmd", choices=["list","get","set"])
    p.add_argument("key", nargs="?")
    p.add_argument("value", nargs="?")
    a = p.parse_args()
    try:
        if a.cmd == "list":
            r = etcd_post(a.endpoint, "/v3/kv/range",
                {"key": b64(P), "range_end": b64(P[:-1]+chr(ord(P[-1])+1))})
            for kv in r.get("kvs",[]):
                k = b64dec(kv["key"]).replace(P,"")
                print(f"{k}  ({len(b64dec(kv['value']))} bytes)")
        elif a.cmd == "get":
            r = etcd_post(a.endpoint, "/v3/kv/range", {"key": b64(P+a.key)})
            kvs = r.get("kvs",[])
            if kvs: print(b64dec(kvs[0]["value"]))
            else: print(f"(key not found: {P}{a.key})")
        elif a.cmd == "set":
            if not a.value: p.error("set requires value"); sys.exit(1)
            etcd_post(a.endpoint, "/v3/kv/put",
                {"key": b64(P+a.key), "value": b64(a.value)})
            print(f"set {P}{a.key} OK")
    except Exception as e:
        print(f"错误: {e}", file=sys.stderr); sys.exit(1)
if __name__ == "__main__": main()
