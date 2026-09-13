---
name: worker-direct-engine-launch-window
description: "A worker (SWE-2 session) typed the game executable at a shell for \"quick selftests\" and put a visible game window on the user's desktop; the guard is CCCP_HEADLESS=1 in every worker process environment plus selftest flags implying headless in the engine"
metadata: 
  node_type: memory
  type: project
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T06:53:46.911Z
---

On 2026-09-12 a SWE-2 worker ran `for t in ...-selftest; do "./Cortex Command.exe" -$t; done` in its worktree, bypassing `tools/run_sim_test.py`; selftests that run after WindowMan::Initialize created a real window three times on the user's screen. Briefs and WORKER_RULES already forbade it; instructions alone were not enough.

**Why:** the user's binding rule is that no game window ever reaches the desktop from unattended work; the engine creates its window hidden only when `CCCP_HEADLESS` is set in the environment (WindowMan.cpp:193) or a headless flag is on the command line.

**How to apply:** every worker route must inject `CCCP_HEADLESS=1` into the spawned process environment (`devin_cli/devin_job.py` `devin_env()`, `win_cli/win_job.py` `worker_env()`, `~/.claude/settings.json` env for Claude Code shells); the engine on fixgroup-4 (539c2d9bee) makes every `*-selftest` flag imply headless. When a visible window is reported, list engine processes with `MainWindowHandle` and the parent command line, kill the visible one and its launcher by PID (see [[process-kill-by-pattern-hits-own-shell]]), end the offending session and resume it with a corrective prompt. A headed review for the user needs `CCCP_HEADLESS` unset explicitly.
