#Requires -Version 7.0
<#
.SYNOPSIS
  Collects every hand-test instance's evidence into one folder:
    pwsh tools\handtest\collect_logs.ps1 -Out <dir>

  Reads only the headed instances - D:\mx\handtest\<role>-<n>\. The self-check
  writes under D:\mx\handtest\_selfcheck\ and is never mixed in; pass
  -Source D:\mx\handtest\_selfcheck to pack a self-check run instead.

  For each instance it copies the console logs (console-*.out.log / *.err.log /
  console-*.log), the runner records (run-*\launch.json, run-*\stdout.log),
  match reports (match-report-*.json), kit launch records (kit-launch-*.json),
  autosaves (Autosaves\), match replays (Userdata\Replays\), crash dumps
  (crash*.dmp, AbortCode*.txt), the menu script and the effective Settings.ini
  into <Out>\<instance>\, then writes <Out>\MANIFEST.txt with the build sha and
  dirty flag of the exe's tree, the exe hash, timestamps, the exact command
  lines used, and every copied file with size and mtime.

  reconnect.ticket is deliberately not copied - it is a live rejoin credential.
  Recursive copies never follow junctions/symlinks (an instance's Data is one).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Out,
    [string]$Source = 'D:\mx\handtest'
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Source)) { throw "no hand-test instances found: $Source does not exist" }
$instances = Get-ChildItem $Source -Directory | Where-Object { $_.Name -match '^(host|client)-\d+$' }
if (-not $instances) { throw "no hand-test instances under $Source" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$collected = Get-Date
$manifest = [System.Collections.Generic.List[string]]::new()
$manifest.Add("handtest collect_logs  $($collected.ToUniversalTime().ToString('o'))")
$manifest.Add("source: $Source")
$manifest.Add('')

# Recursive copy that never follows a junction/symlink: a link itself is skipped
# (its target's content is evidence of the build, not of this instance).
function Copy-TreeNoLinks([string]$src, [string]$dest) {
    $count = 0
    Get-ChildItem $src -Force | ForEach-Object {
        if ($_.LinkType) { return }
        if ($_.PSIsContainer) {
            $count += Copy-TreeNoLinks $_.FullName (Join-Path $dest $_.Name)
        } else {
            New-Item -ItemType Directory -Force $dest | Out-Null
            Copy-Item $_.FullName (Join-Path $dest $_.Name) -Force
            $count++
        }
    }
    return $count
}

foreach ($inst in $instances) {
    $dest = Join-Path $Out $inst.Name
    New-Item -ItemType Directory -Force $dest | Out-Null
    $manifest.Add("== $($inst.Name) ==")
    $manifest.Add("runtime: $($inst.FullName)")

    $copyFile = {
        param($src, [string]$destName = '')
        if (-not (Test-Path $src)) { return $false }
        $f = Get-Item $src -Force
        if ($f.LinkType) { return $false }
        Copy-Item $src (Join-Path $dest ($destName ? $destName : $f.Name)) -Force
        return $true
    }
    # Per-launch files (timestamped) and fixed-name files.
    Get-ChildItem $inst.FullName -File -Force | Where-Object {
        $_.Name -match '^(kit-launch-|console-|match-report-|crash|AbortCode|LogConsole\.txt|selftest\.menu\.txt|host-bans\.txt)' -and -not $_.LinkType
    } | ForEach-Object { & $copyFile $_.FullName | Out-Null }
    $effective = Join-Path $inst.FullName 'Userdata\Settings.ini'
    if (Test-Path $effective) { & $copyFile $effective 'Settings.ini.effective' | Out-Null }
    # Runner records from every run-* directory.
    Get-ChildItem $inst.FullName -Directory -Force | Where-Object { $_.Name -match '^run-' -and -not $_.LinkType } |
        ForEach-Object { Copy-TreeNoLinks $_.FullName (Join-Path $dest $_.Name) | Out-Null }
    # Evidence directories. reconnect.ticket is skipped: it is a live rejoin credential.
    foreach ($dir in 'Autosaves', 'ScreenShots', 'Userdata\Replays') {
        $src = Join-Path $inst.FullName $dir
        if (Test-Path $src) { Copy-TreeNoLinks $src (Join-Path $dest (Split-Path $dir -Leaf)) | Out-Null }
    }

    $launches = Get-ChildItem $inst.FullName -Filter 'kit-launch-*.json' -File -ErrorAction SilentlyContinue
    foreach ($kitFile in $launches) {
        $kit = Get-Content $kitFile.FullName -Raw | ConvertFrom-Json
        $exeDir = Split-Path $kit.executable -Parent
        $bsha = (git -C $exeDir rev-parse HEAD 2>$null)
        $dirty = (git -C $exeDir status --porcelain 2>$null) ? 'dirty' : 'clean'
        $manifest.Add("launch $($kit.launched_utc): exe=$($kit.executable)")
        $manifest.Add("  exe sha256: $($kit.exe_sha256)")
        $manifest.Add("  build sha: $bsha ($dirty)")
        $manifest.Add("  argv: $($kit.argv -join ' ')")
        $manifest.Add("  port: $($kit.port)   fake_lag_ms: $($kit.fake_lag_ms)   mode: $($kit.mode)")
    }
    Get-ChildItem $inst.FullName -Directory -Filter 'run-*' -ErrorAction SilentlyContinue | ForEach-Object {
        $lj = Join-Path $_.FullName 'launch.json'
        if (Test-Path $lj) {
            $j = Get-Content $lj -Raw | ConvertFrom-Json
            $manifest.Add("  $($_.Name): exit_code=$($j.exit_code) pid=$($j.pid) headless_env=$($j.headless_env)")
        }
    }
    $manifest.Add("files copied:")
    Get-ChildItem $dest -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($dest.Length + 1)
        $manifest.Add(("  {0}  {1} bytes  mtime {2}" -f $rel, $_.Length, $_.LastWriteTimeUtc.ToString('o')))
    }
    $manifest.Add('')
}

$manifest | Set-Content (Join-Path $Out 'MANIFEST.txt') -Encoding utf8
Write-Host "[handtest] collected $($instances.Count) instance(s) -> $Out"
Write-Host "[handtest] manifest: $(Join-Path $Out 'MANIFEST.txt')"
