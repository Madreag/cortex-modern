---
name: read-the-clock-before-every-timestamp
description: "Never estimate a UTC stamp for STATUS/RESUME/SPAWN_LOG entries; run `date -u` first — the lead's estimated stamps drifted 90 min ahead in one session"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T18:00:10.016Z
---

On 2026-09-12 the lead stamped STATUS.md, RESUME.md and SPAWN_LOG.md entries by estimation after one real clock check; over an hour the stamps drifted up to 90 minutes ahead of the real clock (entries written at 17:35-17:59 UTC carried 18:00-19:25). The records audit (AUDIT-4) checks stamps against evidence, and the user reads the boards for timing.

**Why:** many tool calls per real minute make elapsed time feel longer than it is; the runner's spawn lines and build logs carry the real clock and expose the drift.

**How to apply:** before writing any timestamp into a board or log, run `date -u` in the same turn and use that value; when a stamp is later found wrong, append a correction note rather than silently rewriting history. Related: [[subagent-report-lives-in-notification]].
