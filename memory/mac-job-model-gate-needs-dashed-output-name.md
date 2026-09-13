---
name: mac-job-model-gate-needs-dashed-output-name
description: "mac_job.py only verifies the Grok model line if the lane's zsh writes agent-output-<x>.jsonl (with a dash); agent-output.jsonl silently skips the gate"
metadata: 
  node_type: memory
  type: project
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T19:44:17.695Z
---

`reviews/takeover-20260909/grok-workers/mac_cli/mac_job.py` checks the CLI's init line with `head -c 600 '<lane>'/agent-output-*.jsonl`; a lane script that redirects to `agent-output.jsonl` (no dash) never matches, so the "Extra High Fast" gate is skipped without any message (the W-MAC-FG5 script had that shape). 

**Why:** the model gate is the only proof a Mac lane ran at Extra High Fast; a silent skip leaves the spawn row unverifiable.

**How to apply:** in every Mac lane zsh, redirect the CLI stream to `agent-output-<lane>.jsonl`; confirm the runner log prints `model verified:` before logging the spawn. Related: [[subagent-report-lives-in-notification]].
