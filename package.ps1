# Builds the DLL, packs the addon zip for the Swofty launcher and installs it into the launcher's addon store.
#   .\package.ps1                       build, pack, install (the install is skipped while F1_25.exe is running)
#   .\package.ps1 -NoInstall            build and pack only
#   .\package.ps1 -InstallWhileRunning  install although the game is running (it keeps the DLL it loaded until its next start)
#   .\package.ps1 -Test                 run tests\run_tests.ps1 first and stop when a test fails
param([switch]$NoInstall, [switch]$InstallWhileRunning, [switch]$Test)
$ErrorActionPreference = "Stop"
$manifestPath = "$PSScriptRoot\mod\swofty.json"
$manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$ver = $manifest.version
# The manifest checks come first, before the build. The option rules are the ones the launcher enforces (swofty dev
# check, 1.8.12); the 255 byte text rule is one an older launcher refused to start with (abff16b), dev check ignores it.
$ids = @($manifest.options | ForEach-Object { $_.id })
if (@($ids | Select-Object -Unique).Count -ne $ids.Count) { throw "swofty.json declares an option id twice" }
foreach ($option in $manifest.options) {
    if ($option.type -eq 'toggle' -and $option.default -isnot [bool]) {
        throw "toggle $($option.id) needs a true/false default"
    }
    if ($option.type -ne 'choice') { continue }
    $values = @($option.values | ForEach-Object {
        if ($_ -is [string]) { $_ } else { $_.value }
    })
    if ($values.Count -lt 1 -or $values.Count -gt 16) {
        throw "choice $($option.id) must have 1..16 values"
    }
    if (@($values | Select-Object -Unique).Count -ne $values.Count) {
        throw "choice $($option.id) lists a value twice"
    }
    if ($option.default -cnotin $values) {
        throw "choice $($option.id) has a default outside its values"
    }
}
# Every string literal of the file, read from the raw text: Windows PowerShell 5.1's ConvertTo-Json spells an apostrophe
# with six characters, and a quote inside a string would split it in two.
$long = [regex]::Matches((Get-Content $manifestPath -Raw -Encoding UTF8), '"(?:[^"\\]|\\.)*"') |
    Where-Object { [Text.Encoding]::UTF8.GetByteCount($_.Value) - 2 -gt 255 }
if ($long) { throw "swofty.json has a string over 255 bytes: $($long[0].Value.Substring(0, 70))..." }
# The version the DLL logs at the start is the manifest's (tests/manifest_presets_test.cpp checks it too).
$sources = @("$PSScriptRoot\mod\native\src\terminal_damage_probe.cpp") + @(Get-ChildItem "$PSScriptRoot\mod\native\src\probe\*.inc" | ForEach-Object { $_.FullName })
if (-not (Select-String -Path $sources -SimpleMatch "loaded version=$ver," -Quiet)) {
    throw "the probe sources do not log 'loaded version=$ver,': change swofty.json and the log line together"
}
if ($Test) {
    & "$PSScriptRoot\tests\run_tests.ps1"
    if ($LASTEXITCODE -ne 0) { throw "tests failed, nothing built, packed or installed" }
}
# build.ps1 puts the repo's zig 0.15.2 on PATH (.tools\zig-x86_64-windows-0.15.2) and refuses any other version.
& "$PSScriptRoot\mod\native\build.ps1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE), nothing packed or installed" }
$stage = Join-Path $env:TEMP "terminal-damage-mod"
$stage = [IO.Path]::GetFullPath($stage)
$tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\', '/')
if ((Split-Path $stage) -ne $tempRoot -or (Split-Path $stage -Leaf) -ne 'terminal-damage-mod') {
    throw "unexpected staging directory: $stage"
}
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stage
if (Test-Path $stage) { throw "could not clear $stage (a file in it is in use?)" }
# Only what the manifest names plus the README; the source lives on GitHub.
New-Item -ItemType Directory -Force "$stage\native" | Out-Null
Copy-Item "$PSScriptRoot\mod\swofty.json", "$PSScriptRoot\mod\README.md", "$PSScriptRoot\mod\icon.png", "$PSScriptRoot\mod\LICENSE" $stage
Copy-Item "$PSScriptRoot\mod\native\terminal_damage_probe.dll" "$stage\native"
New-Item -ItemType Directory -Force "$stage\assets\radio" | Out-Null
Copy-Item "$PSScriptRoot\mod\assets\radio\speech_recognition_voice_true.xml" "$stage\assets\radio"
Copy-Item "$PSScriptRoot\mod\assets\radio\speech_recognition_voice_false.xml" "$stage\assets\radio"
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
$addon = Join-Path $env:LOCALAPPDATA "Swofty\addons\$($manifest.id)"
$running = [bool](Get-Process -Name F1_25 -ErrorAction SilentlyContinue)
if ($NoInstall) {
    Write-Host "packed only (-NoInstall), nothing installed"
} elseif ($running -and -not $InstallWhileRunning) {
    Write-Warning "F1_25.exe is running: packed, NOT installed. Close the game and run package.ps1 again (or pass -InstallWhileRunning)."
} elseif (Test-Path (Split-Path $addon)) {
    # /R:2 /W:1: robocopy's default is a million retries 30 s apart, which would hang here without a word.
    robocopy $stage $addon /MIR /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "install into $addon failed: robocopy $LASTEXITCODE" }
    $LASTEXITCODE = 0
    $got = (Get-Content "$addon\swofty.json" -Raw | ConvertFrom-Json).version
    if ($got -ne $ver) { throw "installed version is $got, expected $ver" }
    Write-Host "installed $ver into $addon"
} else {
    Write-Warning "no Swofty store at $(Split-Path $addon): packed only, nothing installed"
}
