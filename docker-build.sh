#!/bin/bash

set -e

echo "=== Thunder Framework Docker Build ==="

# 检查 Docker
if ! command -v docker &> /dev/null; then
    echo "❌ Docker not found. Please install Docker."
    exit 1
fi

# 检查 Docker daemon
if ! docker ps &> /dev/null; then
    echo "❌ Docker daemon not running. Please start Docker."
    exit 1
fi

echo "✓ Docker is running"

# 构建镜像
echo ""
echo "📦 Building Docker image..."
docker build -t thunder:latest .

# 运行容器
echo ""
echo "🚀 Starting Thunder server..."
docker run -p 8080:8080 \
    --name thunder-server \
    -v $(pwd)/logs:/app/logs \
    thunder:latest

echo "✅ Thunder server is running on http://localhost:8080"
echo "Press Ctrl+C to stop"
