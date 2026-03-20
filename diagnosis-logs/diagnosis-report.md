# Thunder Docker 构建问题诊断报告

生成时间: 2026-03-20 00:46:08
项目目录: d:\interview-quicker1\thunder

## 问题总结
主要问题: Docker 构建时 pip 安装 gtest2html 失败
错误信息: `pip3 install git+https://github.com/abhinav-upadhyay/gtest2html.git` 失败

## 根本原因
1. **网络/认证问题**: Docker 容器内无法访问 GitHub
2. **缺少容错机制**: 安装失败导致整个构建失败
3. **Docker 缓存**: 缓存了失败的构建层

## 已实施的修复
1. **容错 pip 安装命令**:
   ```dockerfile
   RUN pip3 install --upgrade pip && \
       pip3 install gcovr && \
       (pip3 install git+https://github.com/abhinav-upadhyay/gtest2html.git --timeout 60 || echo "gtest2html installation failed, will use alternative") && \
       pip3 install jinja2 lxml  # 用于替代的 HTML 报告生成
   ```

2. **确保 git 可用**:
   ```dockerfile
   RUN apt-get update && apt-get install -y \
       libssl3 \
       libev4 \
       curl \
       python3 \
       python3-pip \
       lcov \
       libgtest-dev \
       git \  # 新增：支持从 GitHub 安装包
       && rm -rf /var/lib/apt/lists/*
   ```

## 验证步骤
```bash
# 1. 清除 Docker 缓存
docker builder prune -a -f
docker rmi thunder-test-builder 2>/dev/null || true

# 2. 运行修复后的构建
python one-click-build-run.py --clean-build --no-reports --output-dir ./test-fix

# 3. 检查构建日志
# 确认 pip 安装命令是容错版本
```

## 备用方案
如果问题仍然存在，可以:
1. 使用离线安装包
2. 跳过 gtest2html 安装（使用内置的 HTML 报告生成器）
3. 配置 Docker 代理设置
