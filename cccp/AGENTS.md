# Instructions pointer — cortex-modern

**This worktree has no instructions of its own. Read the canonical pair instead:**

1. **`D:\Projects\RESUME.md`** — the single live resume doc: current state, the exact next actions, the rules, how
   to build and verify, how to spawn every worker route, lessons learned, architecture, history, path index.
2. **`D:\Projects\CLAUDE.md`** (identical twin: **`D:\Projects\AGENTS.md`**) — the binding policy, rewritten
   2026-09-12 18:10 MST.

`CLAUDE.md` and `AGENTS.md` are byte-identical on purpose — read whichever your tool loads. Worktree-local copies
drift (the copy that used to live here was three months stale on 2026-09-05), so this file stays a pointer.

## The rules you must not violate before reading the above

- **No AI attribution anywhere** — commits, code, docs, reports, PRs, Discord. Author of record is the user. The
  history was scrubbed of the last attribution line on 2026-09-12 and force-pushed: a clone older than
  2026-09-12 17:45 MST must `git fetch --all` and reset its branches to origin.
- **Arizona local time only** (MST, UTC-7, no daylight saving) in every message, board, plan, report and stamp;
  never UTC.
- **Commit as you go through `reviews/takeover-20260909/grok-workers/git_commit.py`; push every verified checkpoint;
  no upstream PRs** (the feed is held).
- **A gate is green only when that exact state was BUILT and RUN and its real output recorded.** Never mark green by
  reasoning; a worker's "PASS" is a claim the lead re-derives from the raw evidence.
- **Workers never push, never touch the milestone branch or the approved tree, never launch the engine outside the
  runners, never move or delete anything under `D:\mx`.**
- **The lead writes RESUME.md, handoffs, plans and verdicts itself; it never delegates them.**
- **Every worker launches with full permissions and resumes its own session** (user, 2026-09-13 16:42 MST); the brief is the
  fence. Since 2026-09-13 GPT-6 Astra through the Codex CLI (`reviews/takeover-20260909/grok-workers/codex_cli/codex_job.py`)
  is the main engineering counterpart; routes and spawn steps: `RESUME.md` §0.5 and §6.3b; the live order of work: §5.0.

