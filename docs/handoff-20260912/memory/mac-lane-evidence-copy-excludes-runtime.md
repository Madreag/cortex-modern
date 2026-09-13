---
name: mac-lane-evidence-copy-excludes-runtime
description: "Copying a Mac worker lane's runs/ with scp -r follows the runtime/Data symlinks and pulls the whole game data; use tar over ssh with --exclude=runtime"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T10:40:35.286Z
---

Mac lane run directories (`/Users/erol/cortex-workers/<lane>/runs/<case>/runtime`) hold a `Data` symlink into the clone; `scp -q -r` follows it and copied 512 MB of game data in three minutes (2026-09-12, W95-mac evidence) while the real evidence was under 1 MB.

**Why:** scp has no exclude and dereferences symlinks; the evidence the lead needs is stdout.log, argv.txt, env.txt, candidates/verdict files, proofs/, harness/, the report and the logs.

**How to apply:** `ssh Erol-Mac 'cd <lane> && tar czf - --exclude=runtime --exclude=gns-src --exclude=scratch runs proofs harness *.log REPORT.md ...' | tar xzf -` into the Windows lane directory; check `du -sh` on the Mac first (`du -sh runs` reports symlink sizes, not targets). Related: [[subagent-report-lives-in-notification]].
