$ErrorActionPreference = 'Stop'
$env:CCCP_HEADLESS = '1'
$env:PYTHONUNBUFFERED = '1'
$env:PYTHONDONTWRITEBYTECODE = '1'
try {
    & '{{PYTHON}}' '{{DRIVER}}' --payload '{{ROOT}}/payload.json' > '{{ROOT}}/payload.log' 2>&1
    $payloadExit = $LASTEXITCODE
} catch {
    $_ | Out-File -LiteralPath '{{ROOT}}/task-error.log'
    $payloadExit = 1
}
Set-Content -LiteralPath '{{ROOT}}/task-exit.txt' -Value $payloadExit
exit $payloadExit
