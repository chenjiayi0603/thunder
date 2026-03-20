#!/usr/bin/env python3
"""
诊断 git 缺失问题
"""

import os
import sys
import subprocess
import time
from pathlib import Path

def check_dockerfile_git():
    """检查 Dockerfile 中的 git 安装"""
    print("检查 Dockerfile 中的 git 安装...")
    
    dockerfile = Path("Dockerfile.test")
    if not dockerfile.exists():
        print(f"[FAIL] Dockerfile 不存在: {dockerfile}")
        return False
    
    with open(dockerfile, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # 检查构建阶段（第一个 FROM ubuntu:22.04 AS builder）
    builder_section = content.split("FROM ubuntu:22.04 AS builder")[1].split("FROM ubuntu:22.04")[0]
    if "git" in builder_section and "apt-get install" in builder_section:
        print(f"[OK] 构建阶段包含 git 安装")
    else:
        print(f"[FAIL] 构建阶段缺少 git 安装")
        return False
    
    # 检查运行阶段（第二个 FROM ubuntu:22.04）
    runtime_section = content.split("FROM ubuntu:22.04")[2]  # 第二个 FROM ubuntu:22.04
    if "git" in runtime_section and "apt-get install" in runtime_section:
        print(f"[OK] 运行阶段包含 git 安装")
    else:
        print(f"[FAIL] 运行阶段缺少 git 安装")
        return False
    
    # 检查 gtest2html 安装
    if "git+https://github.com/abhinav-upadhyay/gtest2html.git" in content:
        print(f"[OK] 包含 gtest2html 从 GitHub 安装")
    else:
        print(f"[FAIL] 缺少 gtest2html 安装")
        return False
    
    return True

def clean_docker_cache():
    """清理 Docker 缓存"""
    print("\n清理 Docker 缓存...")
    
    try:
        # 清理构建缓存
        print("[INFO] 清理 Docker 构建缓存...")
        result = subprocess.run(
            ["docker", "builder", "prune", "-a", "-f"],
            capture_output=True,
            text=True
        )
        if result.returncode == 0:
            print(f"[OK] Docker 构建缓存清理完成")
        else:
            print(f"[WARN] Docker 构建缓存清理失败: {result.stderr}")
        
        # 清理系统缓存
        print("[INFO] 清理 Docker 系统缓存...")
        result = subprocess.run(
            ["docker", "system", "prune", "-a", "-f"],
            capture_output=True,
            text=True
        )
        if result.returncode == 0:
            print(f"[OK] Docker 系统缓存清理完成")
        else:
            print(f"[WARN] Docker 系统缓存清理失败: {result.stderr}")
        
        return True
        
    except Exception as e:
        print(f"[FAIL] Docker 缓存清理异常: {e}")
        return False

def test_docker_git_installation():
    """测试 Docker 容器中的 git 安装"""
    print("\n测试 Docker 容器中的 git 安装...")
    
    try:
        # 创建一个简单的 Dockerfile 测试 git
        test_dockerfile = Path("test-git.dockerfile")
        test_dockerfile_content = """FROM ubuntu:22.04
RUN apt-get update && apt-get install -y git && rm -rf /var/lib/apt/lists/*
CMD ["git", "--version"]
"""
        
        with open(test_dockerfile, "w", encoding="utf-8") as f:
            f.write(test_dockerfile_content)
        
        # 构建测试镜像
        print("[INFO] 构建测试镜像...")
        build_cmd = [
            "docker", "build",
            "-f", "test-git.dockerfile",
            "-t", "test-git-image",
            "."
        ]
        
        result = subprocess.run(
            build_cmd,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"[FAIL] 测试镜像构建失败: {result.stderr}")
            test_dockerfile.unlink(missing_ok=True)
            return False
        
        print(f"[OK] 测试镜像构建成功")
        
        # 运行测试容器
        print("[INFO] 运行测试容器检查 git...")
        run_cmd = [
            "docker", "run", "--rm", "test-git-image"
        ]
        
        result = subprocess.run(
            run_cmd,
            capture_output=True,
            text=True
        )
        
        if result.returncode == 0 and "git version" in result.stdout:
            print(f"[OK] Docker 容器中的 git 可用: {result.stdout.strip()}")
            git_available = True
        else:
            print(f"[FAIL] Docker 容器中的 git 不可用: {result.stderr}")
            git_available = False
        
        # 清理测试镜像
        try:
            subprocess.run(["docker", "rmi", "test-git-image"], 
                         capture_output=True, text=True)
        except:
            pass
        
        # 删除测试 Dockerfile
        test_dockerfile.unlink(missing_ok=True)
        
        return git_available
        
    except Exception as e:
        print(f"[FAIL] Docker git 测试异常: {e}")
        return False

def test_gtest2html_installation():
    """测试 gtest2html 安装"""
    print("\n测试 gtest2html 安装...")
    
    try:
        # 创建一个测试 Dockerfile
        test_dockerfile = Path("test-gtest2html.dockerfile")
        test_dockerfile_content = """FROM ubuntu:22.04
RUN apt-get update && apt-get install -y git python3 python3-pip && rm -rf /var/lib/apt/lists/*
RUN pip3 install git+https://github.com/abhinav-upadhyay/gtest2html.git
CMD ["python3", "-c", "import gtest2html; print('gtest2html import successful')"]
"""
        
        with open(test_dockerfile, "w", encoding="utf-8") as f:
            f.write(test_dockerfile_content)
        
        # 构建测试镜像
        print("[INFO] 构建 gtest2html 测试镜像...")
        build_cmd = [
            "docker", "build",
            "-f", "test-gtest2html.dockerfile",
            "-t", "test-gtest2html-image",
            "."
        ]
        
        result = subprocess.run(
            build_cmd,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"[FAIL] gtest2html 测试镜像构建失败: {result.stderr}")
            print(f"[INFO] 构建输出: {result.stdout}")
            test_dockerfile.unlink(missing_ok=True)
            return False
        
        print(f"[OK] gtest2html 测试镜像构建成功")
        
        # 运行测试容器
        print("[INFO] 运行测试容器检查 gtest2html...")
        run_cmd = [
            "docker", "run", "--rm", "test-gtest2html-image"
        ]
        
        result = subprocess.run(
            run_cmd,
            capture_output=True,
            text=True
        )
        
        if result.returncode == 0 and "gtest2html import successful" in result.stdout:
            print(f"[OK] gtest2html 安装成功")
            gtest2html_available = True
        else:
            print(f"[FAIL] gtest2html 安装失败: {result.stderr}")
            print(f"[INFO] 输出: {result.stdout}")
            gtest2html_available = False
        
        # 清理测试镜像
        try:
            subprocess.run(["docker", "rmi", "test-gtest2html-image"], 
                         capture_output=True, text=True)
        except:
            pass
        
        # 删除测试 Dockerfile
        test_dockerfile.unlink(missing_ok=True)
        
        return gtest2html_available
        
    except Exception as e:
        print(f"[FAIL] gtest2html 测试异常: {e}")
        return False

def test_one_click_script_step2():
    """测试一键编译脚本的步骤2（Docker镜像构建）"""
    print("\n测试一键编译脚本步骤2（Docker镜像构建）...")
    
    try:
        # 创建一个简化的测试脚本
        test_script = Path("test-step2.py")
        test_script_content = """#!/usr/bin/env python3
import subprocess
import sys

# 只运行步骤2：构建Docker镜像
build_cmd = [
    "docker", "build",
    "-f", "Dockerfile.test",
    "-t", "thunder-test-diagnose",
    "--progress=plain",
    "."
]

print("运行Docker构建命令...")
print(f"命令: {' '.join(build_cmd)}")

result = subprocess.run(
    build_cmd,
    capture_output=True,
    text=True
)

print(f"退出码: {result.returncode}")
if result.returncode != 0:
    print(f"错误输出: {result.stderr}")
    print(f"标准输出: {result.stdout}")
    sys.exit(1)
else:
    print("Docker构建成功")
    sys.exit(0)
"""
        
        with open(test_script, "w", encoding="utf-8") as f:
            f.write(test_script_content)
        
        # 运行测试
        print("[INFO] 运行Docker构建测试...")
        result = subprocess.run(
            [sys.executable, "test-step2.py"],
            capture_output=True,
            text=True
        )
        
        if result.returncode == 0:
            print(f"[OK] Docker镜像构建成功")
            
            # 检查输出中是否有git错误
            if "cannot find command 'git'" in result.stdout.lower() or "no such file or directory: 'git'" in result.stdout.lower():
                print(f"[FAIL] 构建输出中包含git错误")
                success = False
            else:
                print(f"[OK] 构建输出中没有git错误")
                success = True
        else:
            print(f"[FAIL] Docker镜像构建失败")
            print(f"[INFO] 错误: {result.stderr}")
            success = False
        
        # 清理测试镜像
        try:
            subprocess.run(["docker", "rmi", "thunder-test-diagnose"], 
                         capture_output=True, text=True)
        except:
            pass
        
        # 删除测试脚本
        test_script.unlink(missing_ok=True)
        
        return success
        
    except Exception as e:
        print(f"[FAIL] 步骤2测试异常: {e}")
        return False

def main():
    print("=" * 60)
    print("git 缺失问题诊断")
    print("=" * 60)
    
    tests = [
        ("检查 Dockerfile git 安装", check_dockerfile_git),
        ("清理 Docker 缓存", clean_docker_cache),
        ("测试 Docker 容器 git 安装", test_docker_git_installation),
        ("测试 gtest2html 安装", test_gtest2html_installation),
        ("测试一键编译脚本步骤2", test_one_click_script_step2),
    ]
    
    results = []
    for test_name, test_func in tests:
        print(f"\n测试: {test_name}")
        try:
            success = test_func()
            results.append((test_name, success))
        except Exception as e:
            print(f"[FAIL] 测试异常: {e}")
            results.append((test_name, False))
    
    print("\n" + "=" * 60)
    print("诊断结果摘要")
    print("=" * 60)
    
    passed = 0
    failed = 0
    
    for test_name, success in results:
        if success:
            print(f"[OK] {test_name}: 通过")
            passed += 1
        else:
            print(f"[FAIL] {test_name}: 失败")
            failed += 1
    
    print(f"\n总计: {passed} 项通过, {failed} 项失败")
    
    if failed == 0:
        print("\n[SUCCESS] 所有诊断测试通过！git 问题可能已解决。")
        print("\n建议运行完整的一键编译测试:")
        print("  python test_docker_build_simple.py")
        print("  python one-click-build-run.py --clean-build --no-reports --output-dir ./test-diagnose")
        return 0
    else:
        print("\n[WARN] 部分诊断测试失败，需要进一步修复。")
        
        # 提供修复建议
        print("\n修复建议:")
        if not check_dockerfile_git():
            print("1. 检查并修复 Dockerfile.test 中的 git 安装")
        if not test_docker_git_installation():
            print("2. 确保 Docker 容器可以安装 git")
        if not test_gtest2html_installation():
            print("3. 检查 gtest2html 安装问题")
        
        return 1

if __name__ == "__main__":
    sys.exit(main())