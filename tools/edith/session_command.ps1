# One command line in the owner's interactive session on a remote box, started by its session task (EDITH: cortex-session1).
# tools/edith/remote_box.py fills the double-brace fields; the command is python and a driver, which start any engine only
# through the runner (tools/win32_test_runner.py via make_run) on a private hidden desktop.
$ErrorActionPreference = 'Continue'
{{ENV}}
{{PATH}}
Set-Location -LiteralPath {{CWD}}
$argv = {{ARGV}}
"session=$((Get-Process -Id $PID).SessionId) user=$env:USERNAME start=$(Get-Date -Format s)" | Out-File -LiteralPath {{LOG}} -Encoding utf8
& python @argv *>> {{LOG}}
"done rc=$LASTEXITCODE end=$(Get-Date -Format s)" | Out-File -LiteralPath {{DONE}} -Encoding ascii
