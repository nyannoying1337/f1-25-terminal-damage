# Builds the DLL and packs the addon zip that Nick imports in the Swofty launcher.
$ErrorActionPreference = "Stop"
$manifest = Get-Content "$PSScriptRoot\mod\swofty.json" -Raw | ConvertFrom-Json
foreach ($option in $manifest.options) {
    if ($option.type -ne 'choice') { continue }
    $values = @($option.values | ForEach-Object {
        if ($_ -is [string]) { $_ } else { $_.value }
    })
    if ($values.Count -lt 1 -or $values.Count -gt 16) {
        throw "choice $($option.id) must have 1..16 values"
    }
    if ($option.default -cnotin $values) {
        throw "choice $($option.id) has a default outside its values"
    }
}
$zig = Join-Path $PSScriptRoot ".tools\zig-x86_64-windows-0.15.2"
if (Test-Path $zig) {
    $env:PATH = "$zig;$env:PATH"
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot "mod\native\build\zig-cache"
    $env:ZIG_LOCAL_CACHE_DIR = $env:ZIG_GLOBAL_CACHE_DIR
}
& "$PSScriptRoot\mod\native\build.ps1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE), nothing packed or installed" }
$long = (Get-Content "$PSScriptRoot\mod\swofty.json" -Raw | ConvertFrom-Json | ConvertTo-Json -Depth 20 -Compress) -split '"' | Where-Object { [Text.Encoding]::UTF8.GetByteCount($_) -gt 255 }
if ($long) { throw "swofty.json has a string over 255 bytes: $($long[0])" }
$ver = (Get-Content "$PSScriptRoot\mod\swofty.json" -Raw | ConvertFrom-Json).version
$stage = Join-Path $env:TEMP "terminal-damage-mod"
$stage = [IO.Path]::GetFullPath($stage)
$tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\', '/')
if ((Split-Path $stage) -ne $tempRoot -or (Split-Path $stage -Leaf) -ne 'terminal-damage-mod') {
    throw "unexpected staging directory: $stage"
}
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stage
# Only what the manifest names plus the README; the source lives on GitHub.
New-Item -ItemType Directory -Force "$stage\native" | Out-Null
Copy-Item "$PSScriptRoot\mod\swofty.json", "$PSScriptRoot\mod\README.md", "$PSScriptRoot\mod\icon.png", "$PSScriptRoot\mod\LICENSE" $stage
Copy-Item "$PSScriptRoot\mod\native\terminal_damage_probe.dll" "$stage\native"
New-Item -ItemType Directory -Force "$stage\assets\radio" | Out-Null
Copy-Item "$PSScriptRoot\mod\assets\radio\speech_recognition.xml" "$stage\assets\radio"
Copy-Item "$PSScriptRoot\mod\assets\radio\speech_recognition_silent.xml" "$stage\assets\radio"
New-Item -ItemType Directory -Force "$stage\ops" | Out-Null
Copy-Item "$PSScriptRoot\mod\ops\radio-probe-assets.json" "$stage\ops"
# swofty.json at the zip root and forward slashes, as the Swofty addon registry expects.
$out = Join-Path $PSScriptRoot "dist\terminal-damage-$ver.zip"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -ErrorAction SilentlyContinue $out
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open($out, "Create")
try {
    Get-ChildItem -Recurse -File $stage | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $rel) | Out-Null
    }
} finally { $zip.Dispose() }
Write-Host "packed $out"

# Installs it like a launcher import: the import only unpacks the zip into the launcher's addon
# store (settings stay in the profile). The launcher deploys it to the game at the next launch.
$addon = Join-Path $env:LOCALAPPDATA "Swofty\addons\nick.terminal-damage-probe"
if (Test-Path (Split-Path $addon)) {
    robocopy $stage $addon /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "install into $addon failed: robocopy $LASTEXITCODE" }
    $LASTEXITCODE = 0
    $got = (Get-Content "$addon\swofty.json" -Raw | ConvertFrom-Json).version
    if ($got -ne $ver) { throw "installed version is $got, expected $ver" }
    Write-Host "installed $ver into $addon"
}
