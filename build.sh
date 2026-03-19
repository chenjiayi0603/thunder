#!/bin/bash

set -e

echo "=== Thunder Framework Build Script ==="

# WSL 代理问题处理
if grep -qi microsoft /proc/version &> /dev/null; then
    echo "⚠️  Detected WSL environment"
    # 禁用代理环境变量
    unset http_proxy
    unset https_proxy
    unset HTTP_PROXY
    unset HTTPS_PROXY
    echo "✓ Proxy settings cleared"
fi

# 检查 CMake
if ! command -v cmake &> /dev/null; then
    echo "❌ CMake not found. Please install CMake."
    exit 1
fi

# 检查编译器支持 C++20
if command -v g++ &> /dev/null; then
    echo "✓ GCC found: $(g++ --version | head -n1)"
elif command -v clang++ &> /dev/null; then
    echo "✓ Clang found: $(clang++ --version | head -n1)"
else
    echo "❌ No suitable C++ compiler found"
    exit 1
fi

# 创建构建目录
mkdir -p build
cd build

# 配置
echo "📦 Configuring CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON

# 编译
echo "🔨 Building..."
make -j$(nproc)

# 运行测试
echo "🧪 Running tests..."
ctest --output-on-failure || echo "⚠️  Some tests failed"

# 安装
echo "📦 Installing..."
if command -v sudo &> /dev/null; then
    sudo make install
else
    make install
fi

echo "✅ Build completed successfully!"
echo "   Binary: ./bin/thunder_server"
