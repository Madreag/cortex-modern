# modernization-docs

Working documentation, analysis, plan and decision log for the **Cortex-Modern**
modernization effort — our fork of the Cortex Command Community Project.

## What this is

A self-contained HTML wiki. No build step, no server — just open the files.

> **Status (2026-06-05).** Stage 1's deterministic-simulation foundation is **built and verified cross-platform GREEN** (Windows + Linux + macOS — a multi-core run replays bit-for-bit identically to a single-core run). It is packaged into a single numeric PR sequence **1–10**: PRs 1–2 are merged upstream (#279/#280, the LuaJIT track), PRs 3–9 are built and verified in the integrated build, and PR 10 (the CI determinism gates) is in progress. Nothing beyond the two LuaJIT PRs is upstream yet — the foundation goes to the maintainer one PR at a time once PR 10 lands and the build-and-boot gate passes. After that, **Stage 2 — the multiplayer itself** (`multiplayer-revamp`, M6→M10, Controller-sync, north star = PvP rollback) begins.
>
> **The canonical sources of truth live outside this wiki:** the master handoff `D:\Projects\HANDOFF.md` (read its top "⚡ RESUME HERE" block first) and the PR plan `D:\Projects\PRs\PR_ROADMAP.html`. If this wiki and those docs disagree, those docs win. The M3 Q40.24 fixed-point approach is **rejected** — FPU + libm standardization is the cross-platform path.

| File | Purpose |
|------|---------|
| `index.html` | Overview / dashboard — start here |
| `analysis.html` | Full technical analysis of the engine (subsystems, cross-cutting issues, the mod boundary) |
| `roadmap.html` | Tiered modernization roadmap |
| `campaign.html` | The flagship: the case for a real strategic campaign to replace the abandoned Conquest mode |
| `multiplayer.html` | Bringing networked multiplayer back — the full four-tier option analysis |
| `ai.html` | The AI overhaul — the #1 community complaint, the foundation under campaign + multiplayer |
| `tracker.html` | Work-item tracker (status board) |
| `decisions.html` | Architecture/strategy decision log (ADR style) |
| `branches.html` | Per-branch deep-dive of the upstream feature branches |
| `issues.html` | Snapshot of open issues on the upstream repo |
| `prs.html` | Snapshot of open pull requests on the upstream repo |
| `corefall.html` | Cross-pollination ideas mined from the sister project Corefall, split into Lua-mod vs engine-code |
| `assets/style.css` | Shared stylesheet |

## How to view

Open `modernization-docs/index.html` in any browser (works over `file://`).

## How to maintain

This wiki is **git-tracked plain HTML** — the git history *is* the change history.

- **Add a work item:** edit the `WORK_ITEMS` array near the bottom of `tracker.html`, then commit.
- **Record a decision:** add an `<section class="adr">` card to `decisions.html`, then commit.
- **Update analysis/roadmap:** edit the HTML directly, then commit.
- **Refresh upstream issues/PRs:** re-fetch the open items and rewrite the `ISSUES` / `PULLS` arrays in `issues.html` / `prs.html`, then commit. These pages are dated snapshots, not live feeds.

Keep edits small and commit often so the history stays useful.

## Repos

- **Working fork:** https://github.com/Madreag/cortex-modern (this repo)
- **Upstream:** https://github.com/cortex-command-community/Cortex-Command-Community-Project
- **Related project:** https://github.com/Madreag/corefall — a parallel game project; cross-pollination ideas are catalogued in `corefall.html`.

**Branch convention** (stage pivot per ADR-019; PR plan current as of 2026-06-05):

- **Stage 1 active trunk:** `determinism-revamp` (cut fresh from current `upstream/development` @ `20dfb3ea5`) — the determinism foundation, now built + verified cross-platform GREEN and packaged into the numeric PR sequence 3–10 per `D:\Projects\PRs\PR_ROADMAP.html` (canonical) / `DETERMINISM_REVAMP_PLAN.md`. The integrated, verified base is `stage/cumulative-verify`.
- **Stage 2 future trunk:** `multiplayer-revamp` (cut from Stage 1 tip when Stage 1 closes) — M6–M10 MP work, Controller-sync from day one, per `MULTIPLAYER_REVAMP_PLAN.md`.
- **Stage 0 legacy:** `modernization-effort` (cut from `development` @ `41e6b7010`) — preserved as source-of-truth for code being selectively brought forward to revamp trunks. No new milestone work cuts from it.

See `D:/Projects/cccp/CLAUDE.md` "Stage convention" + "Architecturally rejected" + "PR decomposition" sections for the binding rules, and `D:\Projects\HANDOFF.md` for live state. The PR sequence is a single numeric 1–10 — there is no "PR A/B/C/D" or "Wave 1/2/3" naming.
