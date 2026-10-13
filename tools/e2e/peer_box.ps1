param([Parameter(Mandatory=$true)][string]$Request)
$ErrorActionPreference = 'Stop'
$requestData = Get-Content -LiteralPath $Request -Raw | ConvertFrom-Json
$tree = $requestData.box.repo
$out = $requestData.out
$taskName = $requestData.box.task
$sessionDir = $requestData.box.session_dir
$slog = "$out\session.log"; $sdone = "$out\session.done"
if ((Get-ScheduledTask -TaskName $taskName).State -eq 'Running') {
  'done rc=99 assigned game task is busy' | Out-File -LiteralPath $sdone -Encoding ascii
  exit 99
}
if (Get-Process -Name 'Cortex Command' -ErrorAction SilentlyContinue) {
  'done rc=99 another game is running on the host box' | Out-File -LiteralPath $sdone -Encoding ascii
  exit 99
}
$gns = [Environment]::GetEnvironmentVariable('GNS_ROOT', 'User')
$probe = @'
import ctypes as C, ctypes.wintypes as W
u = C.windll.user32
h = u.GetForegroundWindow()
buf = C.create_unicode_buffer(256); u.GetWindowTextW(h, buf, 256)
pid = W.DWORD(0); u.GetWindowThreadProcessId(h, C.byref(pid))
state = C.c_int(0); C.windll.shell32.SHQueryUserNotificationState(C.byref(state))
title = buf.value
clear = (h == 0) or title in ("Program Manager", "") or state.value in (1, 5, 6, 7)
print("FOREGROUND hwnd=%d pid=%d quns=%d title=%r clear=%s" % (h, pid.value, state.value, title, clear))
'@
Set-Content -LiteralPath "$out\foreground_probe.py" -Value $probe -Encoding utf8
$payload = @"
`$ErrorActionPreference = 'Continue'
"session=`$((Get-Process -Id `$PID).SessionId) start=`$(Get-Date -Format s)" | Out-File -LiteralPath '$slog' -Encoding utf8
`$fg = & python '$out\foreground_probe.py' 2>&1 | Out-String
`$fg.Trim() | Out-File -LiteralPath '$slog' -Append -Encoding utf8
if (`$fg -notmatch 'clear=True') { "done rc=98 user program in the foreground; scene not run end=`$(Get-Date -Format s)" | Out-File -LiteralPath '$sdone' -Encoding ascii; exit 0 }
`$env:CCCP_HEADLESS = '1'; `$env:GNS_ROOT = '$gns'; `$env:CC_RUNNER_IGNORE_FULLSCREEN = '1'
Set-Location '$tree'
& python -u '$tree\tools\e2e_peers.py' --play '$Request' *>> '$slog'
"done rc=`$LASTEXITCODE end=`$(Get-Date -Format s)" | Out-File -LiteralPath '$sdone' -Encoding ascii
"@
Set-Content -LiteralPath "$sessionDir\run.ps1" -Value $payload -Encoding utf8
Start-ScheduledTask -TaskName $taskName
$deadline = (Get-Date).AddDays(1)
while (-not (Test-Path -LiteralPath $sdone) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
if (-not (Test-Path -LiteralPath $sdone)) { throw 'Scene task hang guard expired' }
$done = Get-Content -LiteralPath $sdone -Raw
$done.Trim()
Get-Content -LiteralPath $slog -Tail 4
if ($done -match 'rc=(\d+)') { exit [int]$Matches[1] }
throw 'Scene task wrote no exit code'
