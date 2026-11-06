<#
.SYNOPSIS
  Builds and runs the standalone test programs tests\*.cpp (optionally compiles tools\*.cpp too).
.DESCRIPTION
  Every test is its own program (own main, most include the whole probe source). A test passes when it builds and its
  exit code is 0 (the asserts abort on failure). Builds run in parallel; the programs run afterwards, on an idle
  machine, because some tests sleep and read the clock. Nothing is written into the repository: the zig cache, the
  executables and the logs live under $env:TEMP\terminal-damage-tests.
.PARAMETER Name
  Tests by file name without .cpp ("_test" may be left out, wildcards work): -Name power_loss,'*tether*'. Default: all.
.PARAMETER Jobs
  Parallel zig processes. Default: the logical processors, at most 16.
.PARAMETER RunTimeoutSec
  A test program still running after this many seconds is killed and fails. Default 120.
.PARAMETER Tools
  Also build tools\*.cpp and run them once with output files under the out dir (dev tools, no assertions).
.PARAMETER AllowWarnings
  By default a build that prints a compiler warning fails its test (all tests and tools build without warnings
  with -Wall -Wextra). This switch only reports the warnings.
.PARAMETER Detail
  One line per test with the build and run seconds.
.PARAMETER Repo
  The checkout to test. Default: the one this script is in (the public mirror works too).
.PARAMETER OutDir
  Where the zig cache, executables and logs go. Must be outside the repository.
.EXAMPLE
  powershell -NoProfile -File tests\run_tests.ps1
  powershell -NoProfile -File tests\run_tests.ps1 -Name team_model,realistic_failures -Jobs 4 -Detail
#>
[CmdletBinding()]
param(
    [string[]]$Name,
    [int]$Jobs = 0,
    [int]$RunTimeoutSec = 120,
    [switch]$Tools,
    [switch]$AllowWarnings,
    [switch]$Detail,
    [string]$Repo = '',
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
$clock = [Diagnostics.Stopwatch]::StartNew()
# Defaults are computed here: $PSScriptRoot is empty inside param() when the script is started with powershell -File.
if (-not $Repo) { $Repo = Join-Path $PSScriptRoot '..' }
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP 'terminal-damage-tests' }
$repo = (Resolve-Path -LiteralPath $Repo).Path
$OutDir = [IO.Path]::GetFullPath($OutDir)
if ($OutDir.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) { throw "OutDir must be outside the repository: $OutDir" }
if ($Jobs -le 0) { $Jobs = [Math]::Min(16, [Environment]::ProcessorCount) }

# The compiler the DLL is built with (zig 0.15.2): this script's own repo first, then the tested repo, else zig from PATH.
$zig = ''
foreach ($root in @((Join-Path $PSScriptRoot '..'), $repo)) {
    $candidate = Join-Path $root '.tools\zig-x86_64-windows-0.15.2\zig.exe'
    if (-not $zig -and (Test-Path -LiteralPath $candidate)) { $zig = (Resolve-Path -LiteralPath $candidate).Path }
}
if (-not $zig) {
    $found = Get-Command zig -ErrorAction SilentlyContinue
    if (-not $found) { throw 'zig not found: put it on PATH or unpack it to .tools\zig-x86_64-windows-0.15.2' }
    $zig = $found.Source
}
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'tools') | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $OutDir 'zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = $env:ZIG_GLOBAL_CACHE_DIR

# What to run. -File passes "-Name a,b" as one string, so split on commas as well.
$files = @(Get-ChildItem -LiteralPath (Join-Path $repo 'tests') -Filter '*.cpp' | Sort-Object Name)
$unmatched = 0
if ($Name) {
    $picked = @()
    foreach ($p in @($Name | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
        $p = $p -replace '\.cpp$', ''
        $hit = @($files | Where-Object { $_.BaseName -like $p -or $_.BaseName -eq ($p + '_test') })
        if ($hit.Count -eq 0) { Write-Output "no test matches '$p'"; ++$unmatched }
        $picked += $hit
    }
    $files = @($picked | Sort-Object FullName -Unique)
}
$jobsList = @()
foreach ($f in $files) {
    $jobsList += [pscustomobject]@{ Name = $f.BaseName; Src = $f.FullName; Args = @(); Status = ''; Why = ''; Log = ''
                                    BuildSec = 0.0; RunSec = 0.0; Warn = 0 }
}
if ($Tools) {
    # Dev tools: built like the tests and run once; they take output paths, nothing else is asserted.
    $toolArgs = @{ damage_panel_preview = @('game.svg', 'states.svg'); hud_guide = @('hud_guide.svg'); failure_audit = @() }
    foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $repo 'tools') -Filter '*.cpp' | Sort-Object Name)) {
        $a = @(); if ($toolArgs.ContainsKey($f.BaseName)) { $a = @($toolArgs[$f.BaseName] | ForEach-Object { Join-Path $OutDir "tools\$_" }) }
        $jobsList += [pscustomobject]@{ Name = 'tool:' + $f.BaseName; Src = $f.FullName; Args = $a; Status = ''; Why = ''; Log = ''
                                        BuildSec = 0.0; RunSec = 0.0; Warn = 0 }
    }
}
foreach ($j in $jobsList) {
    $stem = $j.Name -replace '[:\\/]', '_'
    $j | Add-Member -NotePropertyName Exe -NotePropertyValue (Join-Path $OutDir "$stem.exe")
    $j | Add-Member -NotePropertyName BuildLog -NotePropertyValue (Join-Path $OutDir "$stem.build.log")
    $j | Add-Member -NotePropertyName RunLog -NotePropertyValue (Join-Path $OutDir "$stem.run.log")
}

function Stop-Tree($proc) { try { & taskkill.exe /PID $proc.Id /T /F 2>&1 | Out-Null } catch { } }

# Runs one process per item, at most $Limit at a time. $Start returns the started process, $Finish(item, process,
# seconds, timedOut) takes the result.
function Invoke-Pool($Items, [scriptblock]$Start, [scriptblock]$Finish, [int]$Limit, [int]$TimeoutSec) {
    $pending = New-Object System.Collections.Queue
    foreach ($i in $Items) { $pending.Enqueue($i) }
    $live = New-Object System.Collections.ArrayList
    while ($pending.Count -gt 0 -or $live.Count -gt 0) {
        while ($pending.Count -gt 0 -and $live.Count -lt $Limit) {
            $item = $pending.Dequeue()
            [void]$live.Add([pscustomobject]@{ Item = $item; Proc = (& $Start $item); Clock = [Diagnostics.Stopwatch]::StartNew() })
        }
        Start-Sleep -Milliseconds 40
        for ($k = $live.Count - 1; $k -ge 0; --$k) {
            $e = $live[$k]
            $late = $false
            if (-not $e.Proc.HasExited) {
                if ($TimeoutSec -le 0 -or $e.Clock.Elapsed.TotalSeconds -le $TimeoutSec) { continue }
                Stop-Tree $e.Proc
                $late = $true
                [void]$e.Proc.WaitForExit(5000)
            }
            $live.RemoveAt($k)
            & $Finish $e.Item $e.Proc $e.Clock.Elapsed.TotalSeconds $late
        }
    }
}

function Start-Logged($file, [string]$arguments, $log) {
    $p = @{ FilePath = $file; WorkingDirectory = $repo; NoNewWindow = $true; PassThru = $true
            RedirectStandardOutput = "$log.out"; RedirectStandardError = "$log.err" }
    if ($arguments) { $p.ArgumentList = $arguments }
    $proc = Start-Process @p
    $null = $proc.Handle  # keep the handle: Windows PowerShell 5.1 loses ExitCode otherwise
    $proc
}
function Join-Log($log) {
    $text = ''
    foreach ($part in "$log.out", "$log.err") { if (Test-Path -LiteralPath $part) { $text += [string](Get-Content -LiteralPath $part -Raw) } }
    Set-Content -LiteralPath $log -Value $text
    foreach ($part in "$log.out", "$log.err") {  # a child may hold the file a moment longer
        for ($try = 0; $try -lt 20; ++$try) {
            try { Remove-Item -LiteralPath $part -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 50 }
        }
    }
    $text
}

# ---- Phase 1: build (same flags as the old sequential loop) ----
Write-Host ("building {0} program(s), {1} at a time ({2})" -f $jobsList.Count, $Jobs, $zig)
$include = Join-Path $repo 'mod\native\include'
Invoke-Pool $jobsList {
    param($j)
    $a = 'c++ -target x86_64-windows-gnu -std=c++23 -O1 -Wall -Wextra "-I{0}" "{1}" -o "{2}" -lws2_32' -f $include, $j.Src, $j.Exe
    $p = Start-Logged $zig $a $j.BuildLog
    try { $p.PriorityClass = 'BelowNormal' } catch { }
    $p
} {
    param($j, $proc, $sec, $late)
    $j.BuildSec = $sec
    $text = Join-Log $j.BuildLog
    $j.Warn = ([regex]::Matches($text, '(?m)^\S.*\bwarning:')).Count
    if ($late) { $j.Status = 'fail'; $j.Why = 'build timed out'; $j.Log = $j.BuildLog }
    elseif ($proc.ExitCode -ne 0) { $j.Status = 'fail'; $j.Why = "build failed (exit $($proc.ExitCode))"; $j.Log = $j.BuildLog }
    elseif (-not $AllowWarnings -and $j.Warn -gt 0) { $j.Status = 'fail'; $j.Why = "$($j.Warn) compiler warning(s) (-AllowWarnings reports them only)"; $j.Log = $j.BuildLog }
    else { $j.Status = 'built' }
} $Jobs 900  # a build running longer than 15 minutes is hung

# ---- Phase 2: run, nothing else is compiling now ----
$toRun = @($jobsList | Where-Object { $_.Status -eq 'built' })
if ($toRun.Count -gt 0) { Write-Host ("running {0} program(s)" -f $toRun.Count) }
Invoke-Pool $toRun {
    param($j)
    Start-Logged $j.Exe (($j.Args | ForEach-Object { '"' + $_ + '"' }) -join ' ') $j.RunLog
} {
    param($j, $proc, $sec, $late)
    $j.RunSec = $sec
    [void](Join-Log $j.RunLog)
    if ($late) { $j.Status = 'fail'; $j.Why = "still running after $RunTimeoutSec s, killed" }
    elseif ($proc.ExitCode -ne 0) {
        $j.Status = 'fail'
        $j.Why = if ($proc.ExitCode -lt 0) { 'exit code 0x{0:X8} (abort or crash)' -f $proc.ExitCode } else { "exit code $($proc.ExitCode)" }
    }
    else { $j.Status = 'pass' }
    $j.Log = $j.RunLog
} $Jobs $RunTimeoutSec

# ---- Report ----
if ($Detail) {
    foreach ($j in $jobsList) { Write-Output ('{0,-5} {1,-34} build {2,5:N1} s  run {3,5:N1} s  warnings {4}' -f $j.Status, $j.Name, $j.BuildSec, $j.RunSec, $j.Warn) }
}
foreach ($j in @($jobsList | Where-Object { $_.Warn -gt 0 })) {
    Write-Output ("warning: {0}: {1} compiler warning(s), first: {2}" -f $j.Name, $j.Warn,
        ((Get-Content -LiteralPath $j.BuildLog | Where-Object { $_ -match '\bwarning:' } | Select-Object -First 1)))
}
foreach ($j in @($jobsList | Where-Object { $_.Status -ne 'pass' })) {
    Write-Output "FAIL $($j.Name): $($j.Why)"
    if ($j.Log -and (Test-Path -LiteralPath $j.Log)) {
        Get-Content -LiteralPath $j.Log -Tail 8 | ForEach-Object { Write-Output "    $_" }
    }
}
$pass = @($jobsList | Where-Object { $_.Status -eq 'pass' }).Count
$fail = $jobsList.Count - $pass + $unmatched
Write-Host ("{0:N1} s, logs and executables in {1}" -f $clock.Elapsed.TotalSeconds, $OutDir)
Write-Output ("pass={0} fail={1}" -f $pass, $fail)
if ($fail -gt 0) { exit 1 }
