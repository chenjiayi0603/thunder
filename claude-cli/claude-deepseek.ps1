#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

# Dedicated launcher: run Claude Code with DeepSeek Anthropic-compatible API.
# Usage (PowerShell):
#   .\scripts\claude-deepseek.ps1
#   .\scripts\claude-deepseek.ps1 --flash
#   .\scripts\claude-deepseek.ps1 --max
#   .\scripts\claude-deepseek.ps1 --flash --max
#   .\scripts\claude-deepseek.ps1 -p --permission-mode default --output-format json "只回复OK，并输出当前模型"

$MODEL = 'deepseek-v4-pro'
$USE_MAX = $false
$claudeArgs = [string[]]@()
foreach ($arg in $args) {
    if ($arg -eq '--flash') {
        $MODEL = 'deepseek-v4-flash'
    } elseif ($arg -eq '--max') {
        $USE_MAX = $true
    } else {
        $claudeArgs += $arg
    }
}

# Fixed key (same as bash).
$KEY = 'sk-90512d21961f41dd94fbea786bd04cbc'

# 默认沿用系统代理；需强制直连见环境变量 CLAUDE_DEEPSEEK_UNSET_PROXY=1
foreach ($k in @(
    'ANTHROPIC_AUTH_TOKEN', 'ANTHROPIC_DEFAULT_HAIKU_MODEL',
    'NODE_EXTRA_CA_CERTS'
)) {
    if (Get-Item -Path "Env:\$k" -ErrorAction SilentlyContinue) {
        Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue
    }
}
if ($env:CLAUDE_DEEPSEEK_UNSET_PROXY -eq '1') {
    foreach ($k in @(
        'http_proxy', 'https_proxy', 'all_proxy', 'HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY'
    )) {
        if (Get-Item -Path "Env:\$k" -ErrorAction SilentlyContinue) {
            Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue
        }
    }
    $env:no_proxy = 'localhost,127.0.0.1'
    $env:NO_PROXY = $env:no_proxy
}

$env:ANTHROPIC_BASE_URL = 'https://api.deepseek.com/anthropic'
$env:ANTHROPIC_API_KEY = $KEY
$env:ANTHROPIC_MODEL = $MODEL

$env:CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC = '1'
$env:CLAUDE_CODE_SKIP_FAST_MODE_NETWORK_ERRORS = '1'
$env:CLAUDE_CODE_SIMPLE = '1'

# Match bash inline JSON; paths must be valid JSON strings for --settings
$settingsHashtable = @{
    env = @{
        ANTHROPIC_BASE_URL                        = 'https://api.deepseek.com/anthropic'
        ANTHROPIC_MODEL                          = $MODEL
        CLAUDE_CODE_SIMPLE                       = '1'
        CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC = '1'
        CLAUDE_CODE_SKIP_FAST_MODE_NETWORK_ERRORS = '1'
    }
}
if ($USE_MAX) {
    $settingsHashtable.thinking = @{ type = 'enabled'; budget_tokens = 32000 }
} else {
    $settingsHashtable.thinking = $true
}
$settingsJson = $settingsHashtable | ConvertTo-Json -Compress -Depth 5

$bare = @('--bare')
if ($env:CLAUDE_DEEPSEEK_NO_BARE -eq '1') { $bare = @() }

# 注意：不要用 -c (--continue)，会与 Linux 启动器行为不一致
$runArgs = $bare + @('--settings', $settingsJson) + $claudeArgs
$exe = Get-Command claude -ErrorAction SilentlyContinue
if (-not $exe) {
    Write-Error "未找到 'claude' 命令。请安装 Claude Code CLI 并加入 PATH 后再试。"
    exit 127
}
& claude @runArgs
exit $LASTEXITCODE
