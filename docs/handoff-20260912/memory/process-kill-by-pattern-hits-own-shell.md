---
name: process-kill-by-pattern-hits-own-shell
description: Stop-Process filtered by a CommandLine pattern kills the current Bash tool shell too (the pattern text is in its -c command); kill by PID from a prior query or filter on the process name
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T06:34:32.126Z
---

`Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like "*<pattern>*" } | Stop-Process` matched the Bash tool's own `bash.exe -c "source ...; <the command text>"` chain because the pattern string is part of the command being run, so the call killed itself mid-way (exit 255) and the rest of the chained script never ran.

**Why:** the Bash tool wraps every command in a bash -c whose text contains whatever pattern I typed; anything after the kill in the same call is lost silently.

**How to apply:** first list candidates (`Select ProcessId, Name, CommandLine`), then kill by explicit PIDs in a separate call, or add `-and $_.Name -in @("python.exe","devin.exe")` so bash/pwsh wrappers never match. Keep bookkeeping (log rows, file patches) in a different call from any kill. See [[bash-heredoc-hook-mangles-backslashes]] for the other Bash tool trap.
