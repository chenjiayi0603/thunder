#!/usr/bin/env python3
"""
Thunder Lua 脚本 上传→下发 工具 (对标 build_and_deploy.py)

功能:
  1. 上传 .lua 文件到 admin-web 制品库 (PVC + MinIO)
  2. 从制品库下发 Lua 到 etcd 触发热重载
  3. 列出制品库 Lua 文件
  4. 列出已部署 Lua 脚本

依赖: python3 (标准库)

用法:
  # 上传 + 下发 (全流程)
  python3 tools/lua_deploy.py --file deploy/HelloHttp/scripts/echo.lua --type HELLO_HTTP

  # 只上传不下发
  python3 tools/lua_deploy.py --file echo.lua --type HELLO_HTTP --no-deploy

  # 下发已有的制品库文件
  python3 tools/lua_deploy.py --deploy echo.lua --type HELLO_HTTP

  # 指定 url_path (默认从文件名自动推导)
  python3 tools/lua_deploy.py --file echo.lua --type HELLO_HTTP --url /hello/my_echo

  # 列出制品库
  python3 tools/lua_deploy.py --type HELLO_HTTP --list

  # 列出已部署
  python3 tools/lua_deploy.py --type HELLO_HTTP --list-deployed

  # 指定 admin-web 地址
  python3 tools/lua_deploy.py --file echo.lua --type HELLO_HTTP --admin http://192.168.3.61:30090

选项:
  --file           .lua 文件路径 (上传)
  --type           节点类型 (e.g. HELLO_HTTP, LOGIC, INTERFACE)
  --url            url_path (默认自动从文件名推导: /hello/echo)
  --admin          admin-web 地址 (默认 http://127.0.0.1:8090)
  --no-deploy      只上传到制品库，不下发
  --deploy         下发指定文件 (需配合 --type)
  --list           列出制品库 Lua 文件
  --list-deployed  列出已部署 Lua 脚本
"""

import argparse, json, os, sys, urllib.request, urllib.error
from pathlib import Path

DEFAULT_ADMIN_URL = "http://192.168.3.61:30090"


def api_request(url: str, method="GET", body=None, data=None) -> dict:
    """调用 admin-web REST API"""
    headers = {}
    req_data = None
    if data is not None:
        req_data = data if isinstance(data, bytes) else data.encode()
        headers["Content-Type"] = "application/octet-stream"
    elif body is not None:
        req_data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=req_data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            raw = resp.read()
            if not raw:
                return {"ok": True}
            return json.loads(raw)
    except urllib.error.HTTPError as e:
        body_text = e.read().decode(errors="replace")[:300] if e.fp else ""
        return {"ok": False, "error": f"HTTP {e.code}: {body_text}"}
    except urllib.error.URLError as e:
        return {"ok": False, "error": str(e.reason)}


def derive_url_path(node_type: str, filename: str) -> str:
    """从文件名自动推导 url_path: echo.lua → /hello/echo"""
    name = Path(filename).stem  # 去掉 .lua
    prefix = node_type.lower().replace("_", "")
    # 如果前缀太长取最后一段: HELLO_HTTP → hello
    if "_" in node_type:
        prefix = node_type.lower().split("_")[-1]  # http
        if prefix == "http":
            prefix = "hello"
    return f"/{prefix}/{name}"


def upload_lua(admin_url: str, node_type: str, file_path: Path) -> dict:
    """上传 .lua 到制品库"""
    filename = file_path.name
    url = f"{admin_url}/api/lua/{node_type}/{filename}"
    print(f"📤 上传: PUT {url}")
    print(f"   文件: {file_path} ({file_path.stat().st_size} bytes)")

    with open(file_path, "rb") as f:
        data = f.read()
    result = api_request(url, method="PUT", data=data)

    if result.get("ok"):
        d = result.get("data", {}) or {}
        print(f"  ✅ 上传成功")
        print(f"     PVC: {d.get('path', '?')}")
        return result
    else:
        print(f"  ❌ 上传失败: {result.get('error', '?')}")
        return result


def deploy_lua(admin_url: str, node_type: str, filename: str, url_path: str = None) -> dict:
    """从制品库下发 Lua 到 etcd"""
    if url_path is None:
        url_path = derive_url_path(node_type, filename)
    url = f"{admin_url}/api/lua/{node_type}/deploy"
    print(f"🚀 下发: POST {url}")
    print(f"   文件: {filename} → url_path: {url_path}")

    result = api_request(url, method="POST",
                         body={"filename": filename, "url_path": url_path})

    if result.get("ok"):
        d = result.get("data", {}) or {}
        previous = d.get("previous")
        old_ver = previous.get("version", "new") if previous else "new"
        print(f"  ✅ 下发成功")
        print(f"     node_type: {d.get('node_type', '?')}")
        print(f"     url_path:  {d.get('url_path', '?')}")
        print(f"     size:      {d.get('size', 0)} bytes")
        print(f"     version:   {old_ver} → new")
        return result
    else:
        print(f"  ❌ 下发失败: {result.get('error', '?')}")
        return result


def list_artifacts(admin_url: str, node_type: str):
    """列出制品库 .lua 文件"""
    url = f"{admin_url}/api/lua/{node_type}/files"
    print(f"\n📋 Lua 制品库 ({node_type}):")
    result = api_request(url)

    if result.get("ok"):
        files = result.get("data", {}).get("files", [])
        if not files:
            print("  (无 Lua 文件)")
            return
        print(f"  {'文件名':<35} {'大小':>10}  {'修改时间'}")
        print(f"  {'-'*35} {'-'*10}  {'-'*20}")
        for f in files:
            name = f.get("filename", "?")
            size = f.get("size", 0)
            if size < 1024:
                sz = f"{size}B"
            elif size < 1048576:
                sz = f"{size/1024:.1f}KB"
            else:
                sz = f"{size/1048576:.1f}MB"
            mtime = f.get("mod_time", "")[:19]
            print(f"  {name:<35} {sz:>10}  {mtime}")
    else:
        print(f"  ❌ 查询失败: {result.get('error', '?')}")


def list_deployed(admin_url: str, node_type: str):
    """列出已部署 Lua 脚本"""
    url = f"{admin_url}/api/lua/{node_type}"
    print(f"\n📋 已部署 Lua ({node_type}):")
    result = api_request(url)

    if result.get("ok"):
        scripts = result.get("data", {}).get("scripts", [])
        if not scripts:
            print("  (无已部署脚本)")
            return
        print(f"  {'脚本名':<25} {'URL Path':<30} {'版本':<8} {'大小'}")
        print(f"  {'-'*25} {'-'*30} {'-'*8} {'-'*6}")
        for s in scripts:
            name = s.get("script_name", "?")
            urlp = s.get("url_path", "?")
            ver = s.get("version", 0)
            content = s.get("script_content", "")
            size = len(content) if content else 0
            if size < 1024:
                sz = f"{size}B"
            else:
                sz = f"{size/1024:.1f}KB"
            print(f"  {name:<25} {urlp:<30} v{ver:<7} {sz}")
    else:
        print(f"  ❌ 查询失败: {result.get('error', '?')}")


def main():
    parser = argparse.ArgumentParser(
        description="Thunder Lua 脚本 上传→下发 工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 上传 + 下发
  %(prog)s --file deploy/HelloHttp/scripts/echo.lua --type HELLO_HTTP

  # 只上传
  %(prog)s --file echo.lua --type HELLO_HTTP --no-deploy

  # 下发已有文件
  %(prog)s --deploy echo.lua --type HELLO_HTTP

  # 查看
  %(prog)s --type HELLO_HTTP --list
  %(prog)s --type HELLO_HTTP --list-deployed

  # 连接远程 admin-web
  %(prog)s --file echo.lua --type HELLO_HTTP --admin http://192.168.3.61:30090
        """)
    parser.add_argument("--file", dest="file_path",
                        help=".lua 文件路径 (上传)")
    parser.add_argument("--type", dest="node_type",
                        help="节点类型 (e.g. HELLO_HTTP, LOGIC)")
    parser.add_argument("--url", dest="url_path",
                        help="url_path (默认自动推导)")
    parser.add_argument("--admin", default=DEFAULT_ADMIN_URL,
                        help=f"admin-web 地址 (默认 {DEFAULT_ADMIN_URL})")
    parser.add_argument("--no-deploy", action="store_true",
                        help="只上传不下发")
    parser.add_argument("--deploy", dest="deploy_file",
                        help="下发已有制品库文件")
    parser.add_argument("--list", action="store_true",
                        help="列出制品库")
    parser.add_argument("--list-deployed", action="store_true",
                        help="列出已部署")

    args = parser.parse_args()
    admin_url = args.admin.rstrip("/")

    # --list 模式
    if args.list:
        if not args.node_type:
            parser.error("--list 需要 --type")
        list_artifacts(admin_url, args.node_type)
        return

    # --list-deployed 模式
    if args.list_deployed:
        if not args.node_type:
            parser.error("--list-deployed 需要 --type")
        list_deployed(admin_url, args.node_type)
        return

    # --deploy 模式 (下发已有制品库文件)
    if args.deploy_file:
        if not args.node_type:
            parser.error("--deploy 需要 --type")
        result = deploy_lua(admin_url, args.node_type,
                            args.deploy_file, args.url_path)
        if not result.get("ok"):
            sys.exit(1)
        return

    # 上传模式 (--file)
    if not args.file_path:
        parser.error("需要 --file、--deploy、--list 或 --list-deployed")
    if not args.node_type:
        parser.error("需要 --type")

    file_path = Path(args.file_path)
    if not file_path.exists():
        print(f"❌ 文件不存在: {file_path}")
        sys.exit(1)
    if not file_path.suffix == ".lua":
        print(f"❌ 不是 .lua 文件: {file_path}")
        sys.exit(1)

    # Step 1: 上传
    result = upload_lua(admin_url, args.node_type, file_path)
    if not result.get("ok"):
        sys.exit(1)

    # Step 2: 下发 (除非 --no-deploy)
    if not args.no_deploy:
        result = deploy_lua(admin_url, args.node_type,
                            file_path.name, args.url_path)
        if not result.get("ok"):
            sys.exit(1)

    print(f"\n\033[32m✨ 完成: {file_path.name} → {args.node_type}\033[0m")


if __name__ == "__main__":
    main()
