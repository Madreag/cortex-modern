#Requires -Version 7.0
<#
.SYNOPSIS
  Collects every hand-test instance's evidence into one folder:
    pwsh tools\handtest\collect_logs.ps1 -Out <dir>

  For each instance under D:\mx\handtest\<role>-<n>\ it copies the console log
  (stdout/stderr), the engine log and runner records (launch.json, stdout.log,
  match-report.json), autosaves (Autosaves\), crash dumps (*.dmp, AbortCode.txt)
  and the effective Userdata\Settings.ini into <Out>\<instance>\, then writes
  <Out>\MANIFEST.txt with the build sha, exe hash, timestamps and the exact
  command line each instance ran.
#>
[CmdletBinding()]
param([Parameter(Mandatory)] [string]$Out)
$ErrorActionPreference = 'Stop'

$scratchRoot = 'D:\mx\handtest'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not (Test-Path $scratchRoot)) { throw "no hand-test instances found: $scratchRoot does not exist" }
$instances = Get-ChildItem $scratchRoot -Directory | Where-Object { $_.Name -match '^(host|client)-\d+$' }
if (-not $instances) { throw "no hand-test instances under $scratchRoot" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$buildSha = (git -C $repo rev-parse HEAD 2>$null)
$collected = Get-Date
$manifest = [System.Collections.Generic.List[string]]::new()
$manifest.Add("handtest collect_logs  $($collected.ToUniversalTime().ToString('o'))")
$manifest.Add("repo HEAD sha: $buildSha  (exe sha256 per instance is the binary identity)")
$manifest.Add('')

foreach ($inst in $instances) {
    $dest = Join-Path $Out $inst.Name
    New-Item -ItemType Directory -Force $dest | Out-Null
    $kitLaunch = Join-Path $inst.FullName 'kit-launch.json'
    $kit = $null
    if (Test-Path $kitLaunch) { $kit = Get-Content $kitLaunch -Raw | ConvertFrom-Json }
    $manifest.Add("== $($inst.Name) ==")
    $manifest.Add("runtime: $($inst.FullName)")
    if ($kit) {
        $manifest.Add("exe: $($kit.executable)")
        $manifest.Add("exe sha256: $($kit.exe_sha256)")
        $manifest.Add("launched utc: $($kit.launched_utc)")
        $manifest.Add("argv: $($kit.argv -join ' ')")
        $manifest.Add("port: $($kit.port)   fake_lag_ms: $($kit.fake_lag_ms)")
    }
    $copied = 0
    $copyFile = {
        param($src)
        if (Test-Path $src) { Copy-Item $src $dest -Force; $script:copied++; return $true }
        return $false
    }
    foreach ($name in 'kit-launch.json', 'console.out.log', 'console.err.log', 'console.log',
                     'match-report.json', 'reconnect.ticket', 'host-bans.txt',
                     'crash.dmp', 'AbortCode.txt', 'LogConsole.txt', 'selftest.menu.txt') {
        & $copyFile (Join-Path $inst.FullName $name) | Out-Null
    }
    foreach ($name in 'stdout.log', 'launch.json', 'wrapper.out.log', 'wrapper.err.log', 'runtime.json') {
        & $copyFile (Join-Path $inst.FullName "run\$name") | Out-Null
    }
    $effective = Join-Path $inst.FullName 'Userdata\Settings.ini'
    if (Test-Path $effective) { Copy-Item $effective (Join-Path $dest 'Settings.ini.effective') -Force; $copied++ }
    foreach ($dir in 'Autosaves', 'ScreenShots', 'Mods', 'Userdata\Replays') {
        $src = Join-Path $inst.FullName $dir
        if (Test-Path $src) {
            $files = Get-ChildItem $src -File -Recurse
            if ($files) {
                Copy-Item $src (Join-Path $dest (Split-Path $dir -Leaf)) -Recurse -Force
                $copied += $files.Count
            }
        }
    }
    $exit = $null
    $launchJson = Join-Path $inst.FullName 'run\launch.json'
    if (Test-Path $launchJson) { $exit = ((Get-Content $launchJson -Raw | ConvertFrom-Json).exit_code) }
    if ($null -ne $exit) { $manifest.Add("exit code: $exit") }
    $manifest.Add("files copied: $copied")
    $manifest.Add('')
}

$manifest | Set-Content (Join-Path $Out 'MANIFEST.txt') -Encoding utf8
Write-Host "[handtest] collected $($instances.Count) instance(s) -> $Out"
Write-Host "[handtest] manifest: $(Join-Path $Out 'MANIFEST.txt')"
