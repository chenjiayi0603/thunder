#!/usr/bin/env python3
"""
Thunder SO 构建→上传→下发 一键工具

功能:
  1. 编译指定 CMake target 生成 .so
  2. 通过 admin-web API 上传 .so 到 PVC + MinIO
  3. 通过 admin-web API 下发 .so 到目标 Pod 并触发热重载

依赖: python3 (标准库), cmake, make

用法:
  # 一键构建+上传+下发 (默认向 admin-web 操作)
  python3 tools/build_and_deploy.py  --target ModuleHelloHttp  --type HelloHttp

  # 只构建不上传
  python3 tools/build_and_deploy.py  --target ModuleHelloHttp  --no-deploy  --upload

  # 只上传本地已有 .so (跳过构建)
  python3 tools/build_and_deploy.py  --so ./path/to/xxx.so  --type HelloHttp

  # 上传后不下发 (只到制品库)
  python3 tools/build_and_deploy.py  --target ModuleHelloHttp  --type HelloHttp  --no-deploy

  # 指定 admin-web 地址 (默认 http://127.0.0.1:8090)
  python3 tools/build_and_deploy.py  --target ModuleHelloHttp  --type HelloHttp  --admin http://192.168.3.61:30090

  # 查看已部署的插件
  python3 tools/build_and_deploy.py  --type HelloHttp  --list

选项:
  --target       CMake target 名称 (e.g. ModuleHelloHttp, ModuleLuaHttp, CmdGetToken)
  --type         插件类型目录 (e.g. HelloHttp, Logic, Interface, HelloHttps, HelloWs)
  --so           已有 .so 文件路径 (跳过构建步骤)
  --admin        admin-web 地址 (默认 http://127.0.0.1:8090)
  --build-dir    CMake 构建目录 (默认 /home/tommychen/thunder/build)
  --no-deploy    只上传到制品库，不下发到节点
  --list         列出已部署插件
  --upload       只上传，不构建 (需配合 --so)

编译说明:
  需要在项目根目录执行过 cmake 初始化:
    mkdir -p build && cd build && cmake .. -DTHUNDER_BUILD_NODE_PLUGINS=ON
"""

import argparse, json, os, sys, subprocess, urllib.request, urllib.error
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build"
DEFAULT_ADMIN_URL = "http://127.0.0.1:8090"


def run(cmd: list, cwd=None) -> subprocess.CompletedProcess:
    """执行命令, 失败时打印错误并退出"""
    print(f"  \033[36m$\033[0m {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  \033[31m❌ 失败 (exit={r.returncode})\033[0m")
        if r.stderr:
            print(f"  stderr: {r.stderr.strip()[:500]}")
        if r.stdout:
            print(f"  stdout: {r.stdout.strip()[:500]}")
        sys.exit(1)
    return r


def api_request(url: str, method="GET", body=None, data=None) -> dict:
    """调用 admin-web REST API"""
    headers = {}
    req_data = None
    if data is not None:
        req_data = data  # raw bytes for PUT upload
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


def find_so(build_dir: Path, target: str) -> Path:
    """在构建目录中查找生成的 .so 文件"""
    # CMake 通常输出到 build/code/<Module>/ 或 build/code/Logic/ 等
    for pattern in [f"**/{target}.so", f"code/**/{target}.so",
                    f"code/*/{target}/{target}.so"]:
        matches = sorted(build_dir.glob(pattern))
        if matches:
            return matches[0]
    # fallback: 按名字模糊匹配
    all_so = sorted(build_dir.glob("**/*.so"))
    for so in all_so:
        if target.lower() in so.stem.lower() and not so.name.startswith("lib"):
            return so
    return None


def build_so(target: str, build_dir: Path) -> Path:
    """编译 CMake target 并返回 .so 路径"""
    if not (build_dir / "CMakeCache.txt").exists():
        print(f"⚠️  构建目录 {build_dir} 未初始化 cmake，正在自动初始化...")
        build_dir.mkdir(parents=True, exist_ok=True)
        run(["cmake", str(PROJECT_ROOT / "code"),
             "-DTHUNDER_BUILD_NODE_PLUGINS=ON",
             "-DCMAKE_BUILD_TYPE=Release"],
            cwd=build_dir)

    print(f"\n📐 编译 target: {target}")
    import multiprocessing
    jobs = str(multiprocessing.cpu_count())
    run(["cmake", "--build", str(build_dir), "--target", target, "-j", jobs],
        cwd=build_dir)

    so_path = find_so(build_dir, target)
    if not so_path:
        print(f"\033[31m❌ 未找到生成的 .so 文件 (target={target})\033[0m")
        print("  构建目录中的 .so 文件:")
        for so in sorted(build_dir.glob("**/*.so")):
            print(f"    {so.relative_to(build_dir)}")
        sys.exit(1)

    print(f"  ✅ 构建成功: {so_path} ({so_path.stat().st_size} bytes)")
    return so_path


def upload_so(admin_url: str, type_dir: str, so_path: Path) -> dict:
    """上传 .so 到 admin-web 制品库 (PVC + MinIO)"""
    filename = so_path.name
    url = f"{admin_url}/api/plugins/{type_dir}/{filename}"
    print(f"\n📤 上传到制品库: PUT {url}")
    print(f"   文件: {so_path} ({so_path.stat().st_size} bytes)")

    with open(so_path, "rb") as f:
        data = f.read()
    result = api_request(url, method="PUT", data=data)

    if result.get("ok"):
        d = result.get("data", {}) or {}
        minio_url = d.get("minio_url", "")
        print(f"  ✅ 上传成功")
        print(f"     PVC: {d.get('path', '?')}")
        if minio_url:
            print(f"     MinIO: {minio_url[:80]}...")
        return result
    else:
        print(f"  ❌ 上传失败: {result.get('error', '?')}")
        return result


def deploy_so(admin_url: str, type_dir: str, filename: str) -> dict:
    """下发 .so 到目标 Pod 并触发热重载"""
    url = f"{admin_url}/api/plugins/{type_dir}/deploy"
    print(f"\n🚀 下发到节点: POST {url}")
    print(f"   文件: {filename}")

    result = api_request(url, method="POST", body={"filename": filename})

    if result.get("ok"):
        d = result.get("data", {}) or {}
        print(f"  ✅ 下发完成")
        print(f"     node_type: {d.get('node_type', '?')}")
        print(f"     pods: {d.get('succeeded', 0)}/{d.get('total_pods', 0)} 成功")
        if d.get("failed", 0) > 0:
            print(f"     \033[33m⚠️  {d['failed']} 个 Pod 失败\033[0m")
        for pod in d.get("pods", []):
            icon = "✅" if pod.get("success") else f"❌ {pod.get('error', '')}"
            print(f"       {pod.get('pod_name', '?')}: {icon}")
        return result
    else:
        print(f"  ❌ 下发失败: {result.get('error', '?')}")
        return result


def list_deployed(admin_url: str, type_dir: str):
    """列出已部署的插件"""
    url = f"{admin_url}/api/plugins/{type_dir}/deployed"
    print(f"\n📋 已部署插件 ({type_dir}):")
    result = api_request(url)

    if result.get("ok"):
        d = result.get("data", {}) or {}
        files = d.get("files", [])
        if not files:
            print("  (无已部署插件)")
            return
        print(f"  {'文件名':<42} {'版本':<10} {'大小':>10}  {'Load'}")
        print(f"  {'-'*42} {'-'*10} {'-'*10}  {'-'*4}")
        for f in files:
            name = f.get("filename", "?")
            ver = f.get("version", "") or "镜像"
            size = f.get("size", 0)
            if size < 1024:
                sz = f"{size}B"
            elif size < 1048576:
                sz = f"{size/1024:.1f}KB"
            else:
                sz = f"{size/1048576:.1f}MB"
            load = "✅" if f.get("load") else "❌"
            print(f"  {name:<42} v{ver:<9} {sz:>10}  {load}")
    else:
        print(f"  ❌ 查询失败: {result.get('error', '?')}")


def main():
    parser = argparse.ArgumentParser(
        description="Thunder SO 构建→上传→下发 一键工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 全流程: 构建 -> 上传 -> 下发
  %(prog)s --target ModuleHelloHttp --type HelloHttp --admin http://192.168.3.61:30090

  # 只构建
  %(prog)s --target ModuleHelloHttp --type HelloHttp --no-deploy

  # 只上传已有 .so
  %(prog)s --so /path/to/xxx.so --type HelloHttp --no-deploy

  # 查看已部署
  %(prog)s --type HelloHttp --list
        """)
    parser.add_argument("--target", help="CMake target 名称 (e.g. ModuleHelloHttp)")
    parser.add_argument("--type", dest="type_dir",
                        help="插件类型目录 (e.g. HelloHttp, Logic)")
    parser.add_argument("--so", dest="so_path",
                        help="已有 .so 文件路径 (跳过构建)")
    parser.add_argument("--admin", default=DEFAULT_ADMIN_URL,
                        help=f"admin-web 地址 (默认 {DEFAULT_ADMIN_URL})")
    parser.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR),
                        help=f"CMake 构建目录 (默认 {DEFAULT_BUILD_DIR})")
    parser.add_argument("--no-deploy", action="store_true",
                        help="只上传到制品库，不下发到节点")
    parser.add_argument("--list", action="store_true",
                        help="列出已部署插件")
    parser.add_argument("--upload", action="store_true",
                        help="只上传 (需配合 --so)")

    args = parser.parse_args()
    admin_url = args.admin.rstrip("/")
    build_dir = Path(args.build_dir)

    # --list 模式: 查询已部署插件
    if args.list:
        if not args.type_dir:
            parser.error("--list 需要 --type")
        list_deployed(admin_url, args.type_dir)
        return

    # --upload 模式: 上传已有文件
    if args.upload:
        if not args.so_path:
            parser.error("--upload 需要 --so")
        if not args.type_dir:
            parser.error("--upload 需要 --type")
        so_path = Path(args.so_path)
        if not so_path.exists():
            print(f"❌ 文件不存在: {so_path}")
            sys.exit(1)
        upload_so(admin_url, args.type_dir, so_path)
        return

    # 默认流程: 构建 → 上传 → 下发
    if not args.target and not args.so_path:
        parser.error("需要 --target 或 --so")
    if not args.type_dir:
        parser.error("需要 --type (e.g. HelloHttp, Logic, Interface)")

    # Step 1: 获取 .so 文件
    if args.so_path:
        so_path = Path(args.so_path)
        if not so_path.exists():
            print(f"❌ 文件不存在: {so_path}")
            sys.exit(1)
        print(f"\n📁 使用已有文件: {so_path}")
    else:
        so_path = build_so(args.target, build_dir)

    # Step 2: 上传
    result = upload_so(admin_url, args.type_dir, so_path)
    if not result.get("ok"):
        sys.exit(1)

    # Step 3: 下发 (除非 --no-deploy)
    if not args.no_deploy:
        result = deploy_so(admin_url, args.type_dir, so_path.name)
        if not result.get("ok"):
            sys.exit(1)

    print(f"\n\033[32m✨ 完成: {so_path.name} → {args.type_dir}\033[0m")


if __name__ == "__main__":
    main()
