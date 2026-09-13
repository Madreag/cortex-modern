---
name: resolve-check-commit-separate-steps
description: "Never chain a resolver script, the marker check and git commit in one shell line; a failed assertion did not stop the commit and conflict markers were committed"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1ccc20c-5de9-4ed0-bdc3-fc51ecd35f88
  modified: 2026-09-12T23:42:29.382Z
---

Run conflict resolution, the `<<<<<<<` marker check, and `git add && git commit` as separate tool calls, each gated on the previous result.

**Why:** On 2026-09-12 an inline heredoc resolver for Actor.cpp failed its shape assertion, but the `git add && git commit` on the next line of the same Bash call ran anyway, so a merge commit with conflict markers landed and four clean merges were stacked on it before the marker grep caught it. Recovery cost a `git reset --hard` and redoing five merges.

**How to apply:** one call resolves and prints the marker count; only if it prints 0 does the next call commit; then [[merge-verify-per-file-multiset]].
