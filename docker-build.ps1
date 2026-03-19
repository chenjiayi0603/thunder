# Thunder Framework Docker Build Script

Write-Host "=== Thunder Framework Docker Build ===" -ForegroundColor Cyan

# 检查 Docker
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host "❌ Docker not found. Please install Docker Desktop." -ForegroundColor Red
    Write-Host "   Download from: https://www.docker.com/products/docker-desktop" -ForegroundColor Gray
    exit 1
}

Write-Host "✓ Docker command found" -ForegroundColor Green

# 检查 Docker daemon
Write-Host "`n🔍 Checking Docker daemon..." -ForegroundColor Yellow
try {
    $dockerInfo = docker info 2>&1
    Write-Host "✓ Docker daemon is running" -ForegroundColor Green
} catch {
    Write-Host "❌ Docker daemon is not running" -ForegroundColor Red
    Write-Host "   Please start Docker Desktop" -ForegroundColor Gray
    exit 1
}

# 构建镜像
Write-Host "`n📦 Building Docker image..." -ForegroundColor Yellow
docker build -t thunder:latest .

if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ Docker build failed" -ForegroundColor Red
    exit 1
}

Write-Host "✓ Image built successfully" -ForegroundColor Green

# 删除旧容器
Write-Host "`n🗑️  Cleaning up old containers..." -ForegroundColor Yellow
docker rm -f thunder-server 2>$null | Out-Null

# 创建日志目录
if (-not (Test-Path logs)) {
    New-Item -ItemType Directory -Path logs | Out-Null
}

# 运行容器
Write-Host "`n🚀 Starting Thunder server..." -ForegroundColor Yellow
docker run -p 8080:8080 `
    --name thunder-server `
    -v "$((Get-Location).Path)\logs:/app/logs" `
    -it `
    thunder:latest

Write-Host "`n✅ Thunder server started!" -ForegroundColor Green
Write-Host "   Access at: http://localhost:8080" -ForegroundColor Cyan
Write-Host "   Logs: ./logs" -ForegroundColor Cyan
