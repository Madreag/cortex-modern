import io

PATH = r"D:\Projects\RESUME.md"
text = io.open(PATH, encoding="utf-8").read()
lines = text.split("\n")

entry = (
    "**2026-09-11 04:58 UTC - CHECKPOINT PUSH (backup before the Grok commit split).** Main `stage2/p4b-interp-lockstep` "
    "pushed `4b39e3028b..c8f8188ae0` (138 commits; tree verified against the family pin `D:/mx/s41b3/pin.json`: "
    "`source_head c8f8188ae0`, exe `bb3cf264b4e7`, build manifest `grouped-build-bb3cf264/build.json`; only the two vendor "
    "`.lib` build outputs differ from HEAD; attribution scan of all 138 messages: 0 hits). `stage2/takeover-next` pushed at its "
    "pre-split tip `60cb698146` (a backup, not a checkpoint; the W1 split lands on top by fast-forward only). New orphan branch "
    "`backup/takeover-fixes-wip-20260910` (`e95662f91a`, 26 files, 3.7 MB) pushed: the exact uncommitted working set of "
    "`takeover-fixes` (`handoff-20260910/` working-tree.patch, untracked.tar.gz, manifest.json, post-positive20.patch, "
    "positive20-base/, README), the positive20 Mac manifest, and RESUME.md/AGENTS.md/CLAUDE.md as of this push. Also pushed, "
    "after an attribution scan (0 hits each): `backup/pre-resplit-223220`, `control-32312c71e`, `review/source41-preview`, "
    "`stage2/takeover-fixes`. Every local branch with unique history is now on origin (checked with `git branch -r --contains`). "
    "Remaining failures at this checkpoint, unchanged: 13 unreviewed Source41 breadth non-pass cases (`D:/mx/s41b3`), "
    "`mp_snapshot_p5` red pending the comparator repair, `peer_roundtrip` blocked, `heal_global_d0/d3` `AI_StuckForTime` "
    "residual, the four long-red H4 gates. No PR. Grok workers W1 (commit split), W2 (breadth triage), W3 (comparator repair) "
    "were running when this was recorded; their reports go under `reviews/takeover-20260909/grok-workers/`.**"
)

marker = "**2026-09-11 00:29 UTC - NEXT ACTIONS"
idx = next(i for i, l in enumerate(lines) if l.startswith(marker))
old_state = "State: main `c8f8188ae0` (Source41) unpushed 138 ahead;"
new_state = "State: main `c8f8188ae0` (Source41), pushed to origin 2026-09-11 04:58 UTC (see the entry above);"
assert old_state in lines[idx]
lines[idx] = lines[idx].replace(old_state, new_state)
lines[idx:idx] = [entry, ""]

io.open(PATH, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
print("inserted at line", idx + 1)
