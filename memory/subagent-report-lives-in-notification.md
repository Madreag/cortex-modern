---
name: subagent-report-lives-in-notification
description: "When a subagent's Write is refused, its report exists only in the completion notification; the task output file can be 0 bytes, so save the report to disk from the notification text at once"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T05:40:09.966Z
---

Opus subagents in this project sometimes cannot write their REPORT.md / VERDICT.md (the harness refuses the Write); the brief's fallback puts the whole report in the final message. The `tasks/<id>.output` file for such an agent was 0 bytes twice (W74-4, V75), so the notification text is the only copy.

**Why:** the lead's rule is that every lane's evidence is on disk for future leads and verifiers; a report that exists only in a compacted conversation is lost.

**How to apply:** when a notification carries a full report, write it verbatim to the lane's REPORT.md / VERDICT.md (with a one-line "saved by the lead" note and the lead's disposition appended) in the same turn, before anything else. See [[bash-heredoc-hook-mangles-backslashes]] for why such long text goes through the Write tool, not a heredoc.
