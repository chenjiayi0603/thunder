#!/usr/bin/env python3
"""
Thunder 框架一键编译运行脚本
功能：在容器中编译 Thunder 项目，运行测试，生成报告，输出二进制文件
"""

import os
import sys
import subprocess
import argparse
import time
import shutil
from pathlib import Path

class ThunderBuildRunner:
    def __init__(self, args):
        self.args = args
        self.project_root = Path.cwd()
        self.output_dir = self.project_root / args.output_dir
        self.build_output_dir = self.output_dir / "build-output"
        self.reports_dir = self.output_dir / "reports"
        self.binaries_dir = self.output_dir / "bin"
        self.logs_dir = self.output_dir / "logs"
        
        # 颜色输出（Windows 控制台可能不支持，使用简单标记）
        self.OK = "[OK]"
        self.FAIL = "[FAIL]"
        self.WARN = "[WARN]"
        self.INFO = "[INFO]"
        
    def print_header(self, message):
        """打印标题"""
        print("\n" + "=" * 60)
        print(f" {message}")
        print("=" * 60)
    
    def print_step(self, step_num, message):
        """打印步骤"""
        print(f"\n{self.INFO} 步骤 {step_num}: {message}")
    
    def check_prerequisites(self):
        """检查前置条件"""
        self.print_header("检查前置条件")
        
        prerequisites = [
            ("Docker", ["docker", "--version"], "Docker 是容器化构建的必需工具"),
            ("Docker Compose", ["docker-compose", "--version"], "Docker Compose 用于管理测试服务"),
            ("Python 3", ["python", "--version"], "Python 3 用于运行此脚本"),
        ]
        
        all_ok = True
        for name, cmd, description in prerequisites:
            try:
                result = subprocess.run(cmd, capture_output=True, text=True, shell=True)
                if result.returncode == 0:
                    version = result.stdout.strip()
                    print(f"{self.OK} {name}: {version}")
                else:
                    print(f"{self.FAIL} {name}: 未安装 ({description})")
                    all_ok = False
            except Exception as e:
                print(f"{self.FAIL} {name}: 检查失败 - {e}")
                all_ok = False
        
        # 检查必要文件
        required_files = [
            ("Dockerfile.test", "测试构建 Dockerfile"),
            ("CMakeLists.txt", "CMake 构建配置"),
            ("integration-test/docker-compose.test.yml", "测试环境配置"),
        ]
        
        for file_path, description in required_files:
            if (self.project_root / file_path).exists():
                print(f"{self.OK} 文件: {file_path}")
            else:
                print(f"{self.FAIL} 文件: {file_path} 不存在 ({description})")
                all_ok = False
        
        return all_ok
    
    def cleanup_previous_build(self):
        """清理之前的构建"""
        self.print_step(1, "清理之前的构建")
        
        if self.args.clean_build:
            print(f"{self.INFO} 清理构建目录: {self.output_dir}")
            if self.output_dir.exists():
                try:
                    shutil.rmtree(self.output_dir)
                    print(f"{self.OK} 清理完成")
                except Exception as e:
                    print(f"{self.WARN} 清理失败: {e}")
            else:
                print(f"{self.INFO} 构建目录不存在，无需清理")
        
        # 创建输出目录
        for directory in [self.output_dir, self.build_output_dir, self.reports_dir, 
                         self.binaries_dir, self.logs_dir]:
            directory.mkdir(parents=True, exist_ok=True)
            print(f"{self.OK} 创建目录: {directory}")
        
        return True
    
    def build_docker_image(self):
        """构建 Docker 镜像"""
        self.print_step(2, "构建 Docker 镜像")
        
        image_name = "thunder-test-builder"
        dockerfile_path = self.project_root / "Dockerfile.test"
        
        print(f"{self.INFO} 构建镜像: {image_name}")
        print(f"{self.INFO} 使用 Dockerfile: {dockerfile_path}")
        
        build_cmd = [
            "docker", "build",
            "-f", str(dockerfile_path),
            "-t", image_name,
            "--progress=plain",
            "."
        ]
        
        log_file = self.logs_dir / "docker-build.log"
        print(f"{self.INFO} 构建日志: {log_file}")
        
        try:
            # 使用二进制模式写入，然后进行编码处理
            with open(log_file, "wb") as log_f:
                log_f.write(f"构建命令: {' '.join(build_cmd)}\n".encode('utf-8'))
                log_f.write(f"开始时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n".encode('utf-8'))
                log_f.write(("-" * 80 + "\n").encode('utf-8'))
                
                process = subprocess.Popen(
                    build_cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    bufsize=1
                )
                
                # 实时输出并记录日志（处理编码问题）
                for raw_line in process.stdout:
                    try:
                        # 尝试解码为UTF-8
                        line = raw_line.decode('utf-8', errors='replace')
                        sys.stdout.write(line)
                        log_f.write(raw_line)  # 写入原始字节
                    except UnicodeDecodeError:
                        # 如果UTF-8失败，尝试其他编码或写入原始字节
                        try:
                            line = raw_line.decode('gbk', errors='replace')
                            sys.stdout.write(line)
                            log_f.write(raw_line)
                        except:
                            # 如果所有解码都失败，写入占位符
                            sys.stdout.write("[无法解码的二进制数据]\n")
                            log_f.write(b"[un-decodable binary data]\n")
                    log_f.flush()
                
                returncode = process.wait()
                log_f.write(f"\n结束时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n".encode('utf-8'))
                log_f.write(f"退出码: {returncode}\n".encode('utf-8'))
            
            if returncode == 0:
                print(f"{self.OK} Docker 镜像构建成功")
                return True
            else:
                print(f"{self.FAIL} Docker 镜像构建失败，退出码: {returncode}")
                return False
                
        except Exception as e:
            print(f"{self.FAIL} Docker 构建过程异常: {e}")
            return False
    
    def run_tests_in_container(self):
        """在容器中运行测试"""
        self.print_step(3, "在容器中运行测试")
        
        # 创建测试运行脚本
        test_script = self.build_output_dir / "run-build-and-test.sh"
        test_script_content = """#!/bin/bash
set -e

echo "=========================================="
echo "Thunder 编译和测试"
echo "=========================================="

# 编译项目
echo "编译 Thunder 项目..."
cd /build
mkdir -p build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DENABLE_TESTS=ON \
    -DENABLE_INTEGRATION_TESTS=ON \
    -DCMAKE_CXX_FLAGS="-fprofile-arcs -ftest-coverage"
make -j$(nproc)

echo "编译完成"

# 复制二进制文件到输出目录
mkdir -p /app/build-output/bin
mkdir -p /app/build-output/lib
cp -r bin/* /app/build-output/bin/ 2>/dev/null || true
cp -r lib/* /app/build-output/lib/ 2>/dev/null || true

# 运行测试
echo "运行测试..."
cd /build/build
if [ -f "./integration_test" ]; then
    ./integration_test --gtest_output=xml:/app/build-output/test-results.xml
else
    echo "警告: 未找到集成测试可执行文件"
fi

echo "测试完成"
"""
        
        with open(test_script, "w", encoding="utf-8") as f:
            f.write(test_script_content)
        test_script.chmod(0o755)
        
        # 运行容器
        container_name = f"thunder-build-{int(time.time())}"
        
        run_cmd = [
            "docker", "run",
            "--name", container_name,
            "--rm",
            "-v", f"{self.build_output_dir}:/app/build-output",
            "-v", f"{self.project_root}:/build",
            "thunder-test-builder",
            "/app/build-output/run-build-and-test.sh"
        ]
        
        log_file = self.logs_dir / "container-test.log"
        print(f"{self.INFO} 容器日志: {log_file}")
        
        try:
            # 使用二进制模式写入，处理编码问题
            with open(log_file, "wb") as log_f:
                log_f.write(f"运行命令: {' '.join(run_cmd)}\n".encode('utf-8'))
                log_f.write(f"开始时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n".encode('utf-8'))
                log_f.write(("-" * 80 + "\n").encode('utf-8'))
                
                process = subprocess.Popen(
                    run_cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    bufsize=1
                )
                
                # 实时输出并记录日志（处理编码问题）
                for raw_line in process.stdout:
                    try:
                        # 尝试解码为UTF-8
                        line = raw_line.decode('utf-8', errors='replace')
                        sys.stdout.write(line)
                        log_f.write(raw_line)  # 写入原始字节
                    except UnicodeDecodeError:
                        # 如果UTF-8失败，尝试其他编码或写入原始字节
                        try:
                            line = raw_line.decode('gbk', errors='replace')
                            sys.stdout.write(line)
                            log_f.write(raw_line)
                        except:
                            # 如果所有解码都失败，写入占位符
                            sys.stdout.write("[无法解码的二进制数据]\n")
                            log_f.write(b"[un-decodable binary data]\n")
                    log_f.flush()
                
                returncode = process.wait()
                log_f.write(f"\n结束时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n".encode('utf-8'))
                log_f.write(f"退出码: {returncode}\n".encode('utf-8'))
            
            if returncode == 0:
                print(f"{self.OK} 容器测试运行成功")
                return True
            else:
                print(f"{self.FAIL} 容器测试运行失败，退出码: {returncode}")
                return False
                
        except Exception as e:
            print(f"{self.FAIL} 容器运行过程异常: {e}")
            return False
    
    def generate_reports(self):
        """生成测试报告"""
        self.print_step(4, "生成测试报告")
        
        if not self.args.generate_reports:
            print(f"{self.INFO} 跳过报告生成（--no-reports 选项）")
            return True
        
        # 检查测试结果文件
        test_results = self.build_output_dir / "test-results.xml"
        if not test_results.exists():
            print(f"{self.WARN} 未找到测试结果文件: {test_results}")
            return True
        
        print(f"{self.INFO} 生成测试报告...")
        
        # 复制测试结果到报告目录
        shutil.copy2(test_results, self.reports_dir / "test-results.xml")
        
        # 尝试生成 HTML 报告
        try:
            # 检查是否安装了 gtest2html
            result = subprocess.run([sys.executable, "-c", "import gtest2html; print('OK')"], 
                                  capture_output=True, text=True)
            if result.returncode == 0:
                html_report = self.reports_dir / "test-report.html"
                cmd = [
                    sys.executable, "-m", "gtest2html",
                    str(self.reports_dir / "test-results.xml"),
                    "-o", str(html_report)
                ]
                
                subprocess.run(cmd, check=False)
                print(f"{self.OK} HTML 测试报告: {html_report}")
            else:
                print(f"{self.WARN} 未安装 gtest2html，跳过 HTML 报告生成")
                print(f"{self.INFO} 安装命令: pip install gtest2html")
        except Exception as e:
            print(f"{self.WARN} 生成 HTML 报告失败: {e}")
        
        # 复制二进制文件
        print(f"{self.INFO} 收集二进制文件...")
        build_bin_dir = self.build_output_dir / "bin"
        if build_bin_dir.exists():
            for item in build_bin_dir.iterdir():
                if item.is_file():
                    shutil.copy2(item, self.binaries_dir / item.name)
                    print(f"{self.OK} 复制: {item.name}")
        
        return True
    
    def collect_outputs(self):
        """收集输出结果"""
        self.print_step(5, "收集输出结果")
        
        print(f"{self.INFO} 输出目录结构:")
        for root, dirs, files in os.walk(self.output_dir):
            level = root.replace(str(self.output_dir), '').count(os.sep)
            indent = ' ' * 2 * level
            print(f"{indent}{os.path.basename(root)}/")
            subindent = ' ' * 2 * (level + 1)
            for file in files:
                print(f"{subindent}{file}")
        
        # 生成摘要文件
        summary_file = self.output_dir / "build-summary.txt"
        with open(summary_file, "w", encoding="utf-8") as f:
            f.write("Thunder 构建摘要\n")
            f.write("=" * 50 + "\n")
            f.write(f"构建时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"输出目录: {self.output_dir}\n\n")
            
            f.write("生成的二进制文件:\n")
            if self.binaries_dir.exists():
                for item in self.binaries_dir.iterdir():
                    if item.is_file():
                        f.write(f"  - {item.name}\n")
            
            f.write("\n生成的报告:\n")
            if self.reports_dir.exists():
                for item in self.reports_dir.iterdir():
                    if item.is_file():
                        f.write(f"  - {item.name}\n")
        
        print(f"{self.OK} 构建摘要: {summary_file}")
        return True
    
    def cleanup_resources(self):
        """清理资源"""
        if not self.args.cleanup_resources:
            return
        
        self.print_step(6, "清理资源")
        
        cleanup_actions = [
            ("Stop and remove containers", ["docker", "ps", "-aq", "--filter", "name=thunder-build-", "|", "xargs", "-r", "docker", "rm", "-f"]),
            ("Remove build image", ["docker", "rmi", "thunder-test-builder"]),
        ]
        
        for description, cmd_parts in cleanup_actions:
            try:
                # 注意：这里使用 shell=True 来处理管道
                cmd = " ".join(cmd_parts) if isinstance(cmd_parts, list) else cmd_parts
                result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
                if result.returncode == 0:
                    print(f"{self.OK} {description}")
                else:
                    print(f"{self.WARN} {description} 失败（可能资源不存在）")
            except Exception as e:
                print(f"{self.WARN} {description} 异常: {e}")
    
    def run(self):
        """运行完整的构建流程"""
        self.print_header("Thunder 一键编译运行")
        print(f"项目目录: {self.project_root}")
        print(f"输出目录: {self.output_dir}")
        print(f"开始时间: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        
        # 检查前置条件
        if not self.check_prerequisites():
            print(f"\n{self.FAIL} 前置条件检查失败，请修复问题后重试")
            return False
        
        try:
            # 执行构建流程
            steps = [
                self.cleanup_previous_build,
                self.build_docker_image,
                self.run_tests_in_container,
                self.generate_reports,
                self.collect_outputs,
                self.cleanup_resources,
            ]
            
            for i, step in enumerate(steps, 1):
                if not step():
                    print(f"\n{self.FAIL} 步骤 {i} 失败，构建中止")
                    return False
            
            self.print_header("构建完成")
            print(f"{self.OK} Thunder 项目编译和测试成功完成！")
            print(f"{self.INFO} 二进制文件: {self.binaries_dir}")
            print(f"{self.INFO} 测试报告: {self.reports_dir}")
            print(f"{self.INFO} 构建日志: {self.logs_dir}")
            print(f"{self.INFO} 结束时间: {time.strftime('%Y-%m-%d %H:%M:%S')}")
            
            return True
            
        except KeyboardInterrupt:
            print(f"\n{self.WARN} 用户中断构建")
            return False
        except Exception as e:
            print(f"\n{self.FAIL} 构建过程异常: {e}")
            import traceback
            traceback.print_exc()
            return False

def main():
    parser = argparse.ArgumentParser(
        description="Thunder 框架一键编译运行脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
使用示例:
  python one-click-build-run.py                    # 默认选项运行
  python one-click-build-run.py --clean-build      # 清理后重新构建
  python one-click-build-run.py --no-reports       # 不生成报告
  python one-click-build-run.py --output-dir ./my-output  # 指定输出目录
        """
    )
    
    parser.add_argument("--clean-build", action="store_true",
                       help="清理之前的构建结果")
    parser.add_argument("--no-reports", dest="generate_reports", action="store_false",
                       help="不生成测试报告")
    parser.add_argument("--output-dir", default="./thunder-build",
                       help="输出目录（默认: ./thunder-build）")
    parser.add_argument("--cleanup-resources", action="store_true",
                       help="构建完成后清理 Docker 资源")
    
    args = parser.parse_args()
    
    runner = ThunderBuildRunner(args)
    success = runner.run()
    
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()