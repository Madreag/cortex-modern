---
name: family-precondition-counts-idle-msbuild
description: run_family.py refuses to start while idle MSBuild.exe node-reuse processes exist; stop them before launching the family
metadata: 
  node_type: memory
  type: project
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-14T05:22:12.638Z
---

The Source44-style verification family (`reviews/recovery-2026-09-07/contract-audit/run_family.py`, precondition at ~line 147) counts `MSBuild`, `cl`, `link` and `ninja` processes BY NAME and refuses with "N build process(es) running; the chain build runs alone" — while CLAUDE.md's build-cap rule judges only cl/link. Idle MSBuild node-reuse processes (they linger ~1 h after any build) made the 2026-09-13 22:13 MST launch fail although 0 cl/link and 0 engines were running; a failed launch still writes `family-source<N>.json` (must be moved aside before a relaunch) and the wrapper removes the lock.

**Why:** the family is the serial gate for items 1/2/5; a wasted launch costs minutes and a stale "RUNNING" note in the docs.

**How to apply:** before `run_family.py`, run `Get-Process -Name MSBuild,cl,link,ninja`; if only MSBuild nodes remain and no build is running anywhere, `Stop-Process` them; move any pre-existing `family-source<N>.json/.log` aside with a `.failed-<reason>-<hhmm>` suffix (never delete); re-create the lock; relaunch with a NEW console log name. Related: [[resolve-check-commit-separate-steps]].
