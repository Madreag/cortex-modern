# modernization-docs

Working documentation, analysis, plan and decision log for the **Cortex-Modern**
modernization effort — our fork of the Cortex Command Community Project.

## What this is

A self-contained HTML wiki. No build step, no server — just open the files.

| File | Purpose |
|------|---------|
| `index.html` | Overview / dashboard — start here |
| `analysis.html` | Full technical analysis of the engine (subsystems, cross-cutting issues, the mod boundary) |
| `roadmap.html` | Tiered modernization roadmap |
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

Working branch: `modernization-effort`.
