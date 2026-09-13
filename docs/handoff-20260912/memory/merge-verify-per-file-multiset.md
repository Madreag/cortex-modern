---
name: merge-verify-per-file-multiset
description: "After every merge commit run verify_merge_commit.py; keep-both conflict resolutions edit the conflicted working file, never a stage copy"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T23:42:24.434Z
---

Every merge commit on this project is checked with `reviews/takeover-20260909/grok-workers/lead-tools/verify_merge_commit.py <tree> <merge> <other>` (per file, the +/- line multiset of merge^1..merge must equal merge-base..other). Keep-both resolutions are made on the conflicted WORKING file (`resolve_keep_both.py`), never rebuilt from `git show :2:`.

**Why:** On 2026-09-12 a W131 resolution rebuilt from the pre-merge stage silently dropped two hunks git had already auto-merged (a Run-list entry and an include); the merge would have compiled with the new selftest arm never running. The verifier caught it; a "0 differing files" line is the only acceptable result (a both-sides-identical add deduplicated by git shows as one benign "other-only" include line).

**How to apply:** merge, resolve on the working file, grep for markers, commit, run the verifier, and only then move to the next merge. See [[resolve-check-commit-separate-steps]].
