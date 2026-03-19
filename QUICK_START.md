# Thunder Framework - 快速开始指南

## 前置要求

### Windows
- **CMake 3.12+** — https://cmake.org/download/
- **Visual Studio 2022** 或更新版本（包含 C++20 支持）
- **Git** — https://git-scm.com/

### Linux/macOS
- **CMake 3.12+**
- **GCC 11+** 或 **Clang 13+**（支持 C++20）
- **libssl-dev**、**libev-dev**

## 安装步骤

### 1. 克隆仓库
```bash
cd d:\interview-quicker1
git submodule update --init
cd thunder
```

### 2. Windows 本地构建

**选项 A: PowerShell（推荐）**
```powershell
.\build.ps1
```

**选项 B: Batch 脚本**
```cmd
build-local.bat
```

**选项 C: 手动 CMake**
```cmd
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -DCMAKE_CXX_STANDARD=20
cmake --build . --config Release
ctest -C Release
```

### 3. Linux/macOS 构建
```bash
bash build.sh
./build/bin/thunder_server
```

## 验证安装

```bash
# 进入构建目录
cd build

# 运行单元测试
ctest --output-on-failure

# 预期输出
# Test project build
#   CoTaskTest ........................ Passed
#   HttpAwaitableTest ................ Passed
# 100% tests passed
```

## 常见问题

### "CMake not found"
**Windows:**
```powershell
# 下载并安装 CMake
https://cmake.org/download/

# 或使用 Chocolatey
choco install cmake
```

**Linux:**
```bash
sudo apt-get install cmake
```

### "MSVC compiler not found"
运行 **Developer Command Prompt for Visual Studio** 而不是普通 PowerShell/CMD。

或者添加 MSVC 到 PATH：
```powershell
# Visual Studio 2022 Community
$VSPath = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.3X.XXXXX\bin\Hostx64\x64"
$env:Path += ";$VSPath"
```

### "C++20 not supported"
确保使用 Visual Studio 2017 或更新版本：
```powershell
cmake .. -G "Visual Studio 17 2022"
```

## Docker（可选）

如果 Docker Desktop 无法运行，跳过此步骤。

```bash
# 启动 Docker Desktop
docker build -t thunder:latest .
docker run -p 8080:8080 thunder:latest
```

## 后续步骤

1. ✅ 本地编译并运行测试
2. 📖 阅读 `README.md` 了解框架架构
3. 💻 查看 `code/Hello/src/ModuleHello/StepHttpRequestCo.cpp` 学习协程写法
4. 🚀 修改 `code/Hello/src/main.cpp` 实现自己的应用

## 获取帮助

- 📚 C++20 协程文档：https://en.cppreference.com/w/cpp/language/coroutines
- 📚 Thunder 框架文档：查看 `README.md`
- 🐛 问题报告：提交 Issue

---

**祝您使用愉快！🎉**
