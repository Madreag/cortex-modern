#Requires -Version 7.0
<#
.SYNOPSIS
  Hand-test kit launcher. One command brings up isolated multiplayer instances of
  Cortex Command on this PC: each instance gets its own runtime directory under
  D:\mx\handtest\<role>-<n>\ (own Userdata/Settings.ini, Mods, ScreenShots, Temp,
  and a Data junction to the build's data), so saves and settings never collide.

  Headed (default, for the user at this PC): two windowed 1280x720 instances with
  sound on, placed side by side (host left, client right). The lobby is driven by
  hand through the game's own menu - see HANDTEST.md.

  -Headless (self-check, workers only): every launch goes through
  tools/isolated_launch.py, which runs the engine on a private hidden desktop with
  CCCP_HEADLESS=1. A menu-script drives the real lobby UI (Multiplayer -> Host
  Game / Join Game -> Create Lobby / Connect -> Ready -> Start), a bounded
  -net-match-ticks match runs, and both instances exit 0. No visible window.

  Every flag passed exists in Source/Main.cpp at this tip:
    -headed               Main.cpp:8400  (overrides auto-headless)
    -headless             Main.cpp:8395
    -menu-script <path>   Main.cpp:1207
    -net-match-ticks <n>  Main.cpp:1233  (caps a menu-launched match, Main.cpp:6272)
    -net-match-report <f> Main.cpp:1222
    -net-reconnect-ticket <f> Main.cpp:1101
    -net-host-bans <f>    Main.cpp:1106
    -net-fake-lag <ms>    Main.cpp:1508  (GnsTransport fake send/recv lag)
  There is no engine flag for packet loss: -LossPct sets the environment variable
  CC_TEST_GNS_LOSS_PERCENT (Main.cpp:5056-5071), which only takes effect when the
  engine runs headless with lockstep - it is inert in a headed window.

  No engine flag exists for user directory, window size or sound: isolation comes
  from each instance's working directory and its patched Userdata/Settings.ini.
#>
[CmdletBinding()]
param(
    [string]$Build,
    [ValidateSet('host', 'client', 'both')] [string]$Role = 'both',
    [ValidateSet(1, 2)] [int]$Clients = 1,
    [int]$Port = 47400,
    [int]$FakeLagMs = 0,
    [int]$LossPct = 0,
    [switch]$Mac,
    [switch]$Headless
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Build) { $Build = $repo }
$Build = (Resolve-Path $Build).Path
$exe = Join-Path $Build 'Cortex Command.exe'
$buildData = Join-Path $Build 'Data'
$settingsTemplate = Join-Path $Build 'Userdata\Settings.ini'
if (-not (Test-Path $settingsTemplate)) { $settingsTemplate = Join-Path $repo 'Userdata\Settings.ini' }
foreach ($pair in @(@($exe, 'engine exe'), @($buildData, 'Data'), @($settingsTemplate, 'Settings.ini template'))) {
    if (-not (Test-Path $pair[0])) { throw "missing $($pair[1]): $($pair[0]) - pass -Build <dir containing the Final exe>" }
}
$scratchRoot = 'D:\mx\handtest'
New-Item -ItemType Directory -Force $scratchRoot | Out-Null

# Kit-owned port block: 47400-47419. Every harness/e2e base is >= 47563 (or 41210);
# nothing owns 47400-47419. Only the host binds; clients dial the same port.
$specs = [System.Collections.Generic.List[hashtable]]::new()
if ($Role -in 'host', 'both') { $specs.Add(@{ Role = 'host'; N = 1; Name = 'Host' }) }
if (-not $Mac -and $Role -in 'client', 'both') {
    for ($i = 1; $i -le $Clients; $i++) { $specs.Add(@{ Role = 'client'; N = $i; Name = "Client$i" }) }
}

# Settings pinned on every instance: the template's own values plus the lockstep
# pins the harness uses (tools/feel_measure.py private_settings) and display/audio.
function Set-InstanceSettings([string]$iniPath, [string]$displayName) {
    $text = [IO.File]::ReadAllText($iniPath)
    $pins = [ordered]@{
        'ResolutionX'                 = '1280'
        'ResolutionY'                 = '720'
        'ResolutionMultiplier'        = '1.000000'
        'Fullscreen'                  = '0'
        'EnableVSync'                 = '0'
        'UseMultiDisplays'            = '0'
        'SkipIntro'                   = '1'
        'LaunchIntoActivity'          = '0'
        'DeltaTime'                   = '0.016667'
        'MasterVolume'                = '50.000000'
        'MuteMaster'                  = '0'
        'MusicVolume'                 = '100.000000'
        'MuteMusic'                   = '0'
        'SoundVolume'                 = '100.000000'
        'MuteSounds'                  = '0'
        'MuteAudioOnFocusLoss'        = '0'
        'LocalPrediction'             = '1'
        'LocalPredictionMaxTicks'     = '20'
        'NetworkHostDelayPolicy'      = 'Auto'
        'NetworkInputDelayFrames'     = '0'
        'NetworkSlowPlayerBoundTicks' = '3'
        'NetworkSlowPlayerPolicy'     = 'Substitute'
        'NetworkShowDiagnostics'      = '1'
        'NetworkDisplayName'          = $displayName
    }
    foreach ($key in $pins.Keys) {
        $pattern = "(?m)^(\s*$key\s*=\s*)[^\r\n]*"
        $next = [regex]::Replace($text, $pattern, { param($m) $m.Groups[1].Value + $pins[$key] })
        if ($next -eq $text) { $next = $text + "`n`t$key = $($pins[$key])`n" }
        $text = $next
    }
    [IO.File]::WriteAllText($iniPath, $text, [Text.UTF8Encoding]::new($false))
    return $pins
}

function New-Instance([hashtable]$spec) {
    $name = "$($spec.Role)-$($spec.N)"
    $dir = Join-Path $scratchRoot $name
    foreach ($sub in @('', 'Mods', 'ScreenShots', 'Userdata', 'Temp')) {
        New-Item -ItemType Directory -Force (Join-Path $dir $sub) | Out-Null
    }
    $dataLink = Join-Path $dir 'Data'
    $item = Get-Item $dataLink -ErrorAction SilentlyContinue
    if ($item -and $item.LinkType -eq 'Junction' -and $item.Target -ne $buildData) {
        # Remove the link only (never the target tree) and re-point it at this build's Data.
        $item.Delete()
        $item = $null
    }
    if (-not $item) { New-Item -ItemType Junction -Path $dataLink -Target $buildData | Out-Null }
    $ini = Join-Path $dir 'Userdata\Settings.ini'
    if (-not (Test-Path $ini)) { Copy-Item $settingsTemplate $ini }
    $pins = Set-InstanceSettings $ini $spec.Name
    return @{ Spec = $spec; Dir = $dir; Pins = $pins }
}

function Get-InstanceArgs([hashtable]$inst) {
    $dir = $inst.Dir
    $argv = [System.Collections.Generic.List[string]]::new()
    if ($Headless) {
        $argv.Add('-headless')
        $argv.Add('-menu-script'); $argv.Add((Join-Path $dir 'selftest.menu.txt'))
        $argv.Add('-net-match-ticks'); $argv.Add('1200')
    } else {
        $argv.Add('-headed')
    }
    $argv.Add('-net-match-report'); $argv.Add('match-report.json')
    $argv.Add('-net-reconnect-ticket'); $argv.Add('reconnect.ticket')
    if ($inst.Spec.Role -eq 'client') {
        if ($FakeLagMs -gt 0) { $argv.Add('-net-fake-lag'); $argv.Add([string]$FakeLagMs) }
    } else {
        $argv.Add('-net-host-bans'); $argv.Add('host-bans.txt')
    }
    return , $argv.ToArray()
}

function Write-KitLaunch([hashtable]$inst, [string[]]$argv, [hashtable]$envSet) {
    $record = [ordered]@{
        kit = 'handtest'
        role = $inst.Spec.Role
        instance = "$($inst.Spec.Role)-$($inst.Spec.N)"
        display_name = $inst.Spec.Name
        mode = ($Headless ? 'headless-selfcheck' : 'headed')
        executable = $exe
        exe_sha256 = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
        argv = @($exe) + $argv
        cwd = $inst.Dir
        port = $Port
        fake_lag_ms = ($inst.Spec.Role -eq 'client' ? $FakeLagMs : 0)
        env_set = $envSet
        settings_pinned = $inst.Pins
        launched_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    $record | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $inst.Dir 'kit-launch.json') -Encoding utf8
}

function Write-SelftestScript([hashtable]$inst, [int]$players) {
    $path = Join-Path $inst.Dir 'selftest.menu.txt'
    if ($inst.Spec.Role -eq 'host') {
        $body = @"
wait 60
dump_reconnect
goto_main
wait 10
activate ButtonMainToMultiplayer
wait 15
settext TextMultiplayerName Host
activate ButtonMultiplayerHostGame
wait 15
combo_select ComboHostActivity P4 Alpha Duel - Base.rte
settext TextHostPort $Port
settext TextHostPlayers $players
wait 10
activate ButtonMultiplayerCreate
wait_connected $players 120
wait_remote_ready 120
wait_all_ready
dump_lobby
assert_enabled ButtonMultiplayerStart 1
activate ButtonMultiplayerStart
wait_ms 4000
assert_substate Lobby
dump_lobby
exit
"@
    } else {
        $body = @"
wait 60
dump_reconnect
goto_main
wait 10
activate ButtonMainToMultiplayer
wait 15
settext TextMultiplayerName $($inst.Spec.Name)
activate ButtonMultiplayerJoinGame
wait 15
settext TextJoinAddress 127.0.0.1
settext TextJoinPort $Port
wait 10
activate ButtonMultiplayerConnect
wait_connected $players 120
assert_substate Lobby
dump_lobby
activate ButtonMultiplayerReady
wait 10
dump_lobby
wait_ms 4000
assert_substate Lobby
dump_lobby
exit
"@
    }
    Set-Content $path $body -Encoding ascii
}

function Set-WindowPos([System.Diagnostics.Process]$proc, [int]$x, [int]$y) {
    if (-not ('KitWin32' -as [type])) {
        Add-Type -Name KitWin32 -Namespace HandTest -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetWindowPos(System.IntPtr hWnd, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
'@
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while ([DateTime]::UtcNow -lt $deadline) {
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
            [HandTest.KitWin32]::SetWindowPos($proc.MainWindowHandle, [IntPtr]::Zero, $x, $y, 0, 0, 0x0001) | Out-Null
            return $true
        }
        if ($proc.HasExited) { return $false }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Write-MacInstructions {
    $lanIp = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -match '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[0-1])\.)' -and $_.PrefixOrigin -ne 'WellKnown' } |
        Select-Object -First 1).IPAddress
    if (-not $lanIp) { $lanIp = '<this PC''s LAN IP - run ipconfig>' }
    Write-Host ''
    Write-Host '=== Erol-Mac: LAN peer ===' -ForegroundColor Cyan
    Write-Host "This PC's LAN address: $lanIp    Lobby port: $Port"
    Write-Host 'On the Mac, from a checkout of this tree with a built binary:'
    Write-Host '    cd <tree> && ./build-gns/CortexCommand'
    Write-Host 'In its menu: MULTIPLAYER -> Join Game -> Host IP ' -NoNewline
    Write-Host "$lanIp" -NoNewline -ForegroundColor Yellow
    Write-Host " , Port $Port -> Connect -> Ready."
    Write-Host 'The host must set Players high enough (3 for PC host + PC client + Mac) before Create Lobby.'
    Write-Host ''
}

if ($Headless) {
    $players = 1 + $Clients
    $t0 = Get-Date
    $runners = @()
    $instances = $specs | ForEach-Object { New-Instance $_ }
    $startOne = {
        param($inst)
        $argv = Get-InstanceArgs $inst
        $outDir = Join-Path $inst.Dir 'run'
        New-Item -ItemType Directory -Force $outDir | Out-Null
        $envSet = @{ TEMP = Join-Path $inst.Dir 'Temp'; TMP = Join-Path $inst.Dir 'Temp' }
        if ($LossPct -gt 0 -and $inst.Spec.Role -eq 'client') { $envSet['CC_TEST_GNS_LOSS_PERCENT'] = [string]$LossPct }
        $saved = @{}
        foreach ($k in $envSet.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $envSet[$k]) }
        $py = @('tools\isolated_launch.py', '--out', $outDir, '--cwd', $inst.Dir, '--timeout', '420',
                '--stdout', (Join-Path $inst.Dir 'console.log'), '--', "`"$exe`"") + $argv
        $proc = Start-Process -FilePath 'python' -ArgumentList $py -WorkingDirectory $repo `
            -RedirectStandardOutput (Join-Path $outDir 'wrapper.out.log') `
            -RedirectStandardError (Join-Path $outDir 'wrapper.err.log') `
            -WindowStyle Hidden -PassThru
        foreach ($k in $envSet.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
        Write-KitLaunch $inst $argv $envSet
        Write-Host ("[handtest] {0,-9} pid={1}  runtime={2}" -f $inst.Spec.Role, $proc.Id, $inst.Dir)
        return @{ Inst = $inst; Proc = $proc; OutDir = $outDir }
    }
    foreach ($inst in $instances) {
        # A prior run's session files (reconnect ticket, bans, last match report) change
        # where the menu lands on entry; the self-check always starts from a clean lobby.
        foreach ($stale in 'reconnect.ticket', 'Userdata\reconnect.ticket', 'host-bans.txt',
                          'Userdata\host-bans.txt', 'match-report.json') {
            Remove-Item (Join-Path $inst.Dir $stale) -Force -ErrorAction SilentlyContinue
        }
        Write-SelftestScript $inst $players
        if ($inst.Spec.Role -eq 'host') { $runners += & $startOne $inst }
    }
    # Clients dial only once the host's lobby exists - same gate the e2e driver uses.
    $hostRun = $runners | Where-Object { $_.Inst.Spec.Role -eq 'host' } | Select-Object -First 1
    if ($hostRun -and ($instances | Where-Object { $_.Spec.Role -eq 'client' })) {
        $marker = Join-Path $hostRun.OutDir 'stdout.log'
        $deadline = [DateTime]::UtcNow.AddSeconds(120)
        $created = $false
        while ([DateTime]::UtcNow -lt $deadline -and -not $created) {
            if ((Test-Path $marker) -and (Select-String -Path $marker -Pattern 'activate ButtonMultiplayerCreate ok=1' -Quiet)) { $created = $true }
            elseif ($hostRun.Proc.HasExited) { break }
            else { Start-Sleep -Milliseconds 500 }
        }
        Write-Host ("[handtest] host lobby up: {0}" -f $created)
    }
    foreach ($inst in $instances) {
        if ($inst.Spec.Role -eq 'client') { $runners += & $startOne $inst }
    }
    $fail = $false
    foreach ($r in $runners) {
        $r.Proc.WaitForExit()
        $launchJson = Join-Path $r.OutDir 'launch.json'
        $code = $null
        if (Test-Path $launchJson) { $code = (Get-Content $launchJson -Raw | ConvertFrom-Json).exit_code }
        $report = Join-Path $r.Inst.Dir 'match-report.json'
        $ok = ($code -eq 0)
        Write-Host ("[handtest] {0,-9} exit={1}  match-report={2}  log={3}" -f $r.Inst.Spec.Role,
            ($null -eq $code ? 'n/a' : $code), (Test-Path $report), (Join-Path $r.OutDir 'stdout.log'))
        if (-not $ok) { $fail = $true }
    }
    # Only engines this run started (same exe, spawned after t0) count as leftovers -
    # engines other lanes/users run are not ours to report or kill.
    $leftover = Get-Process -Name 'Cortex Command' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $exe -and $_.StartTime -ge $t0 }
    if ($leftover) { Write-Host "[handtest] WARNING: engine still running: $($leftover.Id -join ', ')"; $fail = $true }
    if ($fail) { Write-Host '[handtest] SELF-CHECK FAILED'; exit 1 }
    Write-Host '[handtest] SELF-CHECK PASS: all instances exited 0 through isolated_launch.py'
    exit 0
}

$instances = $specs | ForEach-Object { New-Instance $_ }
$x = 8
foreach ($inst in $instances) {
    $argv = Get-InstanceArgs $inst
    $envSet = @{ TEMP = Join-Path $inst.Dir 'Temp'; TMP = Join-Path $inst.Dir 'Temp'; CC_TEST_CRASH_DUMP = 'crash.dmp' }
    if ($LossPct -gt 0 -and $inst.Spec.Role -eq 'client') { $envSet['CC_TEST_GNS_LOSS_PERCENT'] = [string]$LossPct }
    $saved = @{}
    foreach ($k in $envSet.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $envSet[$k]) }
    $proc = Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory $inst.Dir `
        -RedirectStandardOutput (Join-Path $inst.Dir 'console.out.log') `
        -RedirectStandardError (Join-Path $inst.Dir 'console.err.log') -PassThru
    foreach ($k in $envSet.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    Write-KitLaunch $inst $argv $envSet
    [void](Set-WindowPos $proc $x 30)
    Write-Host ("[handtest] {0,-9} pid={1}  window@({2},30)  runtime={3}" -f $inst.Spec.Role, $proc.Id, $x, $inst.Dir)
    $x += 1288
}
Write-MacInstructions
Write-Host "Lobby port for this session: $Port  (host enters it in Host Setup; joiners dial it in Join Game)"
Write-Host 'Next: follow HANDTEST.md - Multiplayer -> Host Game / Join Game -> Ready -> Start Match.'
Write-Host 'When finished, collect evidence:  pwsh tools\handtest\collect_logs.ps1 -Out <dir>'
