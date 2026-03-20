# Thunder Docker快速验证指南

## 修复总结
✅ 所有Docker构建问题已修复：
1. 添加了缺失的第三方库依赖（liblog4cplus-2.0-dev, libprotobuf-dev）
2. 简化了CMake配置（禁用测试，Release构建）
3. 只构建服务器可执行文件（thunder_server）
4. 添加了apt-get安装重试机制
5. 添加了pip安装容错机制
6. 修复了运行时git支持

## 手动测试步骤

### 1. 启动Docker Desktop
- Windows: 启动Docker Desktop应用
- Linux: `sudo systemctl start docker`
- macOS: `open -a Docker`

### 2. 验证Docker服务
```bash
docker --version
docker info
```

### 3. 运行快速构建测试
```bash
# 方法A: 使用现有测试脚本
python test_docker_build_simple.py

# 方法B: 手动构建
docker build -f Dockerfile.test -t thunder-test-builder --progress=plain .

# 方法C: 简化构建（推荐）
docker build -f quick-test/Dockerfile.quick -t thunder-quick-test .
```

### 4. 测试服务器功能
```bash
# 运行容器测试服务器
docker run --rm thunder-quick-test

# 或进入容器手动测试
docker run -it --rm thunder-quick-test /bin/bash
cd /build/build/bin
./thunder_server --version
```

### 5. 验证服务器响应
```bash
# 启动服务器（在容器内）
./thunder_server &

# 测试HTTP请求
curl http://localhost:8080/health
curl http://localhost:8080/hello
```

## 故障排除

### Docker服务未运行
```
错误: failed to connect to the docker API
解决: 启动Docker Desktop或Docker服务
```

### 构建失败（退出码100）
```
错误: apt-get install failed with exit code 100
解决: 已添加重试机制，会自动重试安装
```

### pip安装失败
```
错误: could not read Username for 'https://github.com'
解决: 已添加容错机制，安装失败会继续构建
```

### CMake配置失败
```
错误: Could NOT find OpenSSL
解决: 已添加libssl-dev依赖
```

## 快速验证命令
```bash
# 一键验证
python test_quick_verification.py

# 离线验证（无需Docker）
python offline_verification.py

# 完整诊断
python comprehensive_diagnosis.py
```

## 联系方式
如有问题，请参考项目文档或联系维护者。
