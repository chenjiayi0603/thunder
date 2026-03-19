@echo off
REM Thunder Framework Build Script for Windows (Batch)

setlocal enabledelayedexpansion

echo === Thunder Framework Build Script (Windows Batch) ===
echo.

REM 检查 CMake
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: CMake not found. Please install CMake.
    echo Download from: https://cmake.org/download/
    pause
    exit /b 1
)

echo Checking CMake... OK

REM 删除旧的构建目录
if exist build (
    echo Cleaning old build directory...
    rmdir /s /q build
)

REM 创建新的构建目录
mkdir build
cd build

REM 配置
echo.
echo Configuring CMake...
cmake .. ^
    -G "Visual Studio 17 2022" ^
    -DCMAKE_CXX_STANDARD=20 ^
    -DCMAKE_CXX_STANDARD_REQUIRED=ON

if errorlevel 1 (
    echo ERROR: CMake configuration failed
    pause
    exit /b 1
)

REM 编译
echo.
echo Building...
cmake --build . --config Release --parallel

if errorlevel 1 (
    echo ERROR: Build failed
    pause
    exit /b 1
)

REM 运行测试
echo.
echo Running tests...
ctest -C Release --output-on-failure

if errorlevel 1 (
    echo WARNING: Some tests failed
)

echo.
echo Build completed successfully!
echo Binary: .\bin\Release\thunder_server.exe
echo.
pause
