# Windows build of the addon DLL. Needs zig 0.15.2: the repo's copy in .tools\zig-x86_64-windows-0.15.2 is used when
# it exists, else zig on PATH must be 0.15.2 (zig 0.17.0 builds a different DLL). With that compiler the DLL is
# byte-reproducible.
$ErrorActionPreference = "Stop"
$zigVersion = "0.15.2"
$repoZig = Join-Path $PSScriptRoot "..\..\.tools\zig-x86_64-windows-$zigVersion"
if (Test-Path $repoZig) { $env:PATH = "$((Resolve-Path $repoZig).Path);$env:PATH" }
$found = (zig version)
if ($found -ne $zigVersion) {
    throw "zig $found is on PATH, the DLL is built with $zigVersion (unpack it to .tools\zig-x86_64-windows-$zigVersion)"
}
Push-Location $PSScriptRoot  # back where the caller was when the script ends (Set-Location left the shell in mod\native)
try {
    $env:ZIG_GLOBAL_CACHE_DIR = "$PSScriptRoot\build\zig-cache"
    $env:ZIG_LOCAL_CACHE_DIR  = "$PSScriptRoot\build\zig-cache"
    New-Item -ItemType Directory -Force build | Out-Null
    zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Iinclude -shared -s `
        -o terminal_damage_probe.dll src/terminal_damage_probe.cpp -lws2_32
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Remove-Item -ErrorAction SilentlyContinue terminal_damage_probe.lib
    Write-Host "built $PSScriptRoot\terminal_damage_probe.dll"
} finally { Pop-Location }
