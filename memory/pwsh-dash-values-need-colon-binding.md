---
name: pwsh-dash-values-need-colon-binding
description: "When invoking a PowerShell script from bash, a parameter value that itself starts with a dash (e.g. \"-input-script path\") must be passed with the colon form -Param:\"value\", else PowerShell reads the value as the next parameter name"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 33bbc5d3-b3a3-4ba2-9e2b-aa59ec12ad01
  modified: 2026-09-06T09:08:39.953Z
---

`pwsh -File run.ps1 -HostExtraArgs "-input-script D:/x.txt"` fails with "Missing an argument for parameter 'HostExtraArgs'" because PowerShell treats the dash-leading value as a parameter name. Passing it as `-HostExtraArgs:"-input-script D:/x.txt"` binds explicitly and works.

**Why:** cost a wasted fixture run on 2026-09-06 (the stage2_p4 e2e harness takes extra game arguments this way).

**How to apply:** for any harness parameter whose value begins with `-` (extra game args, negative numbers), use the `-Name:"value"` form; inside .ps1 scripts calling other scripts, do the same. Related: [[bash-heredoc-hook-mangles-backslashes]].
