---
name: mac-job-uploads-only-run-zsh
description: "mac_job.py copies only run.zsh and the launchd plist to the Mac lane; prompt.txt/policy.md/WORKER_RULES.md/build.zsh must be scp'd first, stale exit.txt removed, and the local cli_runs dir must not pre-exist"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-14T03:18:00.338Z
---

`grok-workers/mac_cli/mac_job.py <lane> <lane>/run.zsh` uploads ONLY `run.zsh` and the plist (`put()` at lines ~52/64). A run.zsh that reads `prompt.txt` (the Grok gates template) then fails with `no such file or directory: prompt.txt` → cursor-agent "No prompt provided for print mode" → the runner reports NO_INIT (rc=1) after the job ends.

**Why:** the 2026-09-13 20:16 MST mac-gates-8418 launch lost ~3 minutes and a lane slot this way; the earlier 3f65 lane only worked because the files had been scp'd by hand.

**How to apply:** before every `mac_job.py` launch: `scp <lane>/prompt.txt <lane>/policy.md <lane>/WORKER_RULES.md <lane>/build.zsh Erol-Mac:/Users/erol/cortex-workers/<lane>/`, `chmod +x build.zsh`, delete any stale `exit.txt` / `agent-output-*.jsonl` in the Mac lane dir, and rename the local `cli_runs/<lane>` if it exists (the runner refuses an existing dir). Related: [[mac-job-model-gate-needs-dashed-output-name]], [[mac-lane-evidence-copy-excludes-runtime]].
