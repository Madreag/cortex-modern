# One payload in the owner's interactive session on EDITH, started by the cortex-session1 task.
# tools/edith_cross.py fills the {{...}} fields and copies the result to D:/mx/session1/run.ps1.
# The engine starts only through the runner (tools/win32_test_runner.py via make_run) on a private hidden desktop.
$ErrorActionPreference = 'Continue'
$env:CCCP_HEADLESS = '1'
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PYTHONUNBUFFERED = '1'
Set-Location -LiteralPath '{{REPO}}'
"session=$((Get-Process -Id $PID).SessionId) user=$env:USERNAME start=$(Get-Date -Format s)" | Out-File -LiteralPath '{{LOG}}' -Encoding utf8
& python '{{DRIVER}}' --remote-peer '{{SPEC}}' *>> '{{LOG}}'
"done rc=$LASTEXITCODE end=$(Get-Date -Format s)" | Out-File -LiteralPath '{{DONE}}' -Encoding ascii
