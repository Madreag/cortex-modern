---
name: arizona-local-time-only
description: "The user wants every time written in Arizona local time (MST, UTC-7, no DST), never UTC; read the clock before stamping"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-13T00:02:59.258Z
---

Write every timestamp and every mention of a time in Arizona local time, `2026-09-12 17:05 MST` style (24-hour, MST suffix), in messages, STATUS.md, RESUME.md, plans, reports and commit bodies. Never UTC. Arizona does not observe daylight saving, so the offset is always UTC-7.

**Why:** the user said on 2026-09-12 17:00 MST "stop talking in UTC, only use local time, Arizona time", after a night of UTC-stamped boards and messages; the rule is now in CLAUDE.md / AGENTS.md / cccp/AGENTS.md.

**How to apply:** run `date` (local) in the same turn as the stamp; convert older UTC evidence (SPAWN_LOG, LEAD-REVIEW, lane reports written before 2026-09-12 17:00 MST) by subtracting 7 hours when quoting it; machine logs stay as the tools write them.
