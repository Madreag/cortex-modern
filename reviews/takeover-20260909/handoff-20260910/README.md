# Handoff snapshot — 2026-09-10 (recovery after the 02:47 UTC usage-limit stop)

Recorded 2026-09-10 ~22:05 UTC from the on-disk state and the Codex thread `01a07bf2-1ee0-7b63-86f8-21dc6ef8e3cc` (root plus subagents `b2_adversarial_review`, `b2_socket_driver`, `source41_matrix_review`; all three errored at 02:47:05 UTC with `usage_limit_exceeded`, the root turn ended at 02:47:06 UTC). Nothing in the repositories was changed to produce this directory; the two Mac evidence packages below were copied back and the `verify_b2_mac_package.py` pin table gained two entries.

## What was running or half-done when the stop hit

| Item | State at 02:47 UTC | Where it ended up |
|---|---|---|
| Windows Source41 breadth v3 (`D:/mx/s41b3`, approved exe `bb3cf264`) | 65/81 collected | Finished on its own at 02:52:33 UTC, 81/81 collected, `execution_pass=false`: 13 non-pass cases, unreviewed (list in RESUME.md §B, 2026-09-10 22:xx entry). `breadth-run-v3.json` `review_status=REQUIRES_GATE_REVIEW`. |
| Mac A7 group on positive20 (`run_a7_mac.py ... a7-group-positive20 all`) | running | Finished 02:56:24 UTC: all five arms pass on binary `238066fa01d9…`, source unchanged before/after each arm. Packed as `a7-group-positive20-0256` (283 files, sha256 `44eec8b6…`), copied back and hash-verified into `../a7-verified-mac-group-positive20-0256/`. |
| Mac positive20 no-GNS package `native-positive20-nogns-0243` | packed on the Mac, not copied | Copied back and hash-verified into `../native-positive20-nogns-verified-0243/` (81 files, sha256 `f831abdc…`). |
| Root: `NetRecoveryJournal.{h,cpp}` + `NetLockstep.cpp` journal hooks + vcxproj/filters/meson entries | mid-edit, UNBUILT | In the `takeover-fixes` working tree. Isolated in `post-positive20.patch`. |
| `b2_adversarial_review`: A7/B2 journal helpers in `NetA7Journal.*`, host observations in `NetReconnectSession.*`, `NetSession.*`, `NetMatchService.cpp`, `NetReconnectTxCache.*`, `NetLobbySession.cpp` | mid-edit, UNBUILT (last write 02:47:29 UTC) | Same working tree, same patch. Plan: `../b2-review/b2-native-observer-plan-20260910.md`; base copies of the files it edited: `../b2-native-implementation/base/`. |
| `b2_socket_driver`: live-driver package (`../b2-live-group-20260910/CONTRACT_V2.md`, `continuity_oracle.py`, `live_group.py`, `recovery_codec.py`) | Python/doc drafts, never executed | Unchanged. Its three frozen engine repairs (`../b2-ui-native-repair-20260910`, `../reclaim-private-adoption-20260910`, `../craft-fixture-isolation-20260910`) are all inside positive20. |
| `source41_matrix_review`: continuation review | frozen | `../source41-matrix-review/continuation-20260910/review.md` (42/44 pass; `heal_global_d0`/`d3` OPEN_SHARED_STATE_RESIDUAL on the host-only `AI_StuckForTime` NumberValue; `mp_snapshot_p5` still failed, `peer_roundtrip` still blocked). It was tracing that NumberValue when stopped; nothing further was written. |

## Files here

- `manifest.json` — sha256/size/mtime of every modified or untracked file in `D:/Projects/takeover-fixes` (branch `stage2/takeover-next`, HEAD `60cb698146`, diff base = main `c8f8188ae0`), excluding the two rebuilt vendor `.lib` files.
- `working-tree.patch` — `git diff --binary c8f8188ae0` of the tracked files (includes the 12 committed round-start/B2 commits' changes plus the uncommitted work).
- `untracked.tar.gz` — the 16 untracked files (new sources, selftests, fixtures, tools).
- `post-positive20.patch` — the ONLY difference between the verified positive20 revision and the current working tree: 14 modified files + 2 new files, ~650 lines of unbuilt A7/B2 journal and observer instrumentation. `post-positive20-summary.json` lists them; `positive20-base/` holds the positive20 copies fetched from the Mac repository and verified against `../b2-mac-positive20/positive20.manifest.json`.

## Restoring the verified positive20 tree (if ever needed)

Positive20 is the last state proven on the Mac (manifest sha256 `29938cd8ba995ab728c3710d66644f4c01659bb29b4a66954bd7fa78ed14c1ad`, base `c8f8188ae0`). To return the Windows working tree to exactly positive20 without losing the instrumentation WIP: keep this directory, copy each file under `positive20-base/` over its counterpart in `takeover-fixes`, and delete `Source/Network/NetRecoveryJournal.{h,cpp}`. Re-apply `post-positive20.patch` to get the WIP back. The Mac repository `/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive` is also exactly positive20 (`source_mismatches: []` in every positive20 run).

## What positive20 is verified to do (all on the Mac, GCC 13.4, arm64)

- GNS build `238066fa01d9…` (attempt20): ten network CLI selftests, controller-frame, native-graph with `CC_TEST_NET_RECLAIM_DIAG` off and on, and the full A7 group (stagger_seat_survives, silent_socket_bound, leave_ack_dropped, slow_resync_save, coop_hand_back) — evidence `../native-positive20-verified-0244/`, `../a7-verified-mac-group-positive20-0256/`.
- No-GNS build (positive-nogns-attempt2): the same ten selftests, controller-frame and native-graph — `../native-positive20-nogns-verified-0243/`.
- Positive19 (one file less: the craft-fixture isolation in `LuaMan.cpp`) carries the 27-group `NetResyncRuntimeSelfTest` run — `../native-positive19-verified-0238/`; positive18 the strict co-op recovery — `../a7-coop-positive18-verified-0234/`.

Not verified anywhere: a Windows build of this working set, the two-peer Windows gates on it, and the ~650-line post-positive20 instrumentation (never compiled).
