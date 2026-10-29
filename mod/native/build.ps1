# Windows build. Needs zig on PATH:  winget install zig.zig
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$env:ZIG_GLOBAL_CACHE_DIR = "$PSScriptRoot\build\zig-cache"
$env:ZIG_LOCAL_CACHE_DIR  = "$PSScriptRoot\build\zig-cache"
New-Item -ItemType Directory -Force build | Out-Null
zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Iinclude -shared -s `
    -o terminal_damage_probe.dll src/terminal_damage_probe.cpp -lws2_32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Remove-Item -ErrorAction SilentlyContinue terminal_damage_probe.lib
Write-Host "built $PSScriptRoot\terminal_damage_probe.dll"
