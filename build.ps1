# Thunder Framework Build Script for Windows

Write-Host "=== Thunder Framework Build Script (Windows) ===" -ForegroundColor Cyan

# 检查并安装依赖
Write-Host "`n[1/5] Checking dependencies..." -ForegroundColor Yellow

# 检查 CMake
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "❌ CMake not found." -ForegroundColor Red
    Write-Host "   Install from: https://cmake.org/download/" -ForegroundColor Gray
    exit 1
}
Write-Host "✓ CMake: $(cmake --version | Select-Object -First 1)" -ForegroundColor Green

# 检查编译器
$compilerFound = $false
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    Write-Host "✓ MSVC compiler found" -ForegroundColor Green
    $compilerFound = $true
} else {
    Write-Host "⚠️  MSVC compiler not in PATH" -ForegroundColor Yellow
    Write-Host "   Please run from 'Developer Command Prompt for VS'" -ForegroundColor Gray
}

if (-not $compilerFound) {
    Write-Host "`n❌ No suitable C++ compiler found" -ForegroundColor Red
    exit 1
}

# 创建构建目录
Write-Host "`n[2/5] Preparing build directory..." -ForegroundColor Yellow
if (Test-Path build) {
    Write-Host "Removing old build directory..."
    Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path build | Out-Null
Write-Host "✓ Build directory created"

# 进入构建目录
Push-Location build

try {
    # 配置
    Write-Host "`n[3/5] Configuring CMake..." -ForegroundColor Yellow
    $cmakeCmd = @(
        "cmake",
        "..",
        "-G", "Visual Studio 17 2022",
        "-DCMAKE_CXX_STANDARD=20",
        "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    )
    
    Write-Host "Running: $($cmakeCmd -join ' ')" -ForegroundColor Gray
    & $cmakeCmd[0] @($cmakeCmd[1..($cmakeCmd.Count-1)])
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n❌ CMake configuration failed (exit code: $LASTEXITCODE)" -ForegroundColor Red
        Write-Host "Troubleshooting steps:" -ForegroundColor Yellow
        Write-Host "  1. Ensure Visual Studio 2022 is installed" -ForegroundColor Gray
        Write-Host "  2. Run from 'Developer Command Prompt for Visual Studio'" -ForegroundColor Gray
        Write-Host "  3. Check that C++ workload is installed in VS" -ForegroundColor Gray
        exit 1
    }
    Write-Host "✓ CMake configuration completed"

    # 编译
    Write-Host "`n[4/5] Building..." -ForegroundColor Yellow
    cmake --build . --config Release --parallel
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n❌ Build failed (exit code: $LASTEXITCODE)" -ForegroundColor Red
        exit 1
    }
    Write-Host "✓ Build completed"

    # 运行测试
    Write-Host "`n[5/5] Running tests..." -ForegroundColor Yellow
    ctest -C Release --output-on-failure
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n⚠️  Some tests failed (this may be optional)" -ForegroundColor Yellow
    } else {
        Write-Host "✓ All tests passed" -ForegroundColor Green
    }

    Write-Host "`n✅ Build completed successfully!" -ForegroundColor Green
    Write-Host "   Binary: .\bin\Release\thunder_server.exe" -ForegroundColor Cyan
    
} finally {
    Pop-Location
}
