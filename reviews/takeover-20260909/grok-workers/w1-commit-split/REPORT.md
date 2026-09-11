# W1 commit split (attempt 2)

Worktree `D:\Projects\takeover-fixes`, branch `stage2/takeover-next`.
Base `60cb6981462d30400d372ca778d440c82a5bcbe3`. Tip `0e2aa3812f0f015b1c6734649dcb4360b80650e5`.
12 commits. Not pushed. Two vendor `*.lib` files never staged.
Attempt-1 tip remains tagged `w1-split-attempt1` = `aaf7057e5dfef135a9b9f09fbc6831672e091895` (untouched).

Note: `git stash list` has `stash@{0}` on `experiment/cross-arch-combat` (`LuaMan.cpp`). Unrelated to this branch; not popped, dropped, or applied.

## Commits

| hash | subject | n | files |
|---|---|---:|---|
| `407660b40bd27cf17e9a71dc94773b9e7b493417` | Carry player-binding observations on the command wire | 2 | `Source/Network/NetGameCommand.h`, `Source/Network/NetGameCommand.cpp` |
| `2ce183842c6cec402b7350df361dfbc879ad6e2f` | Bound a resync envelope around archive, acks and pending commands | 9 | plan 6 plus `RTEA.vcxproj`, `RTEA.vcxproj.filters`, `Source/Network/meson.build` |
| `8a0fcae45f07dfdca23a67bfcef6fc60ea066190` | Restore complete input, drop-map ownership and exact-target priming | 10 | plan 7 plus the three build files |
| `1f184512ad42a72a88f5790ea161febbe1638ca9` | Restore typed native aliases and isolate craft-fixture clones | 2 | `Source/Managers/LuaMan.h`, `Source/Managers/LuaMan.cpp` |
| `039a42c72ed780ef2b5ffa14821b8e99747544e4` | Skip registry mutation when privately adopting a persisted UniqueID | 2 | `Source/Entities/MovableObject.h`, `Source/Entities/MovableObject.cpp` |
| `debd38630573b82a6f53e41920b4ef783262f997` | Keep ACraft exit selection and AHuman inventory across checkpoint load | 3 | `Source/Entities/ACraft.h`, `Source/Entities/ACraft.cpp`, `Source/Entities/AHuman.cpp` |
| `72fe9734c5da8eb65396e61f59bb6f452638ad96` | Add A7 journal helpers for recovery-arm traces | 5 | `NetA7Journal.h/.cpp` plus the three build files |
| `4a8ac20de3da9b0b817511a6039bb3161556ba96` | Give the host a live F6 panel and codec-17 seat snapshots | 22 | plan 20 plus `RTEA.vcxproj`, `RTEA.vcxproj.filters` |
| `ee2679ee1300705c979acb601f1e81a555d1f540` | Restore peer-local player, UI and aliases after a restore | 15 | unchanged from plan |
| `153e413603b1f76dc2a07d13bdd23566c77049e0` | Initialize saved-game modules and use the lobby shared clock | 8 | unchanged from plan |
| `1d5feacd0fb8c559778d483d2ded3c19df735664` | Record A7 recovery events on session, tickets and ownership | 6 | unchanged from plan |
| `0e2aa3812f0f015b1c6734649dcb4360b80650e5` | Compare snapshots by the controlled actor's inventory role | 2 | `tools/compare_snapshots.py`, `tools/test_snapshot_inventory_roles.py` |

The attempt-1 commit "Add the new network and moderation sources to the build" is gone. Messages otherwise unchanged (`scratch/commit-msgs/01.txt`–`12.txt`).
Results: `scratch/commit-results-attempt2.json`. Line map: `scratch/build-lines-map.json`.

## Build entries

Line numbers are in the FINAL (positive20) file. Source: `scratch/build-entries.md` and `scratch/build-lines-map.json`.
`FINAL-minus-all-new-entries == BASE` for all three files. Last-touch intermediate == FINAL (`meson.build` at commit 7, the two vcxproj files at commit 8).

### 2. Bound a resync envelope around archive, acks and pending commands

`RTEA.vcxproj`
- L852: `<ClInclude Include="Source\Network\NetResyncState.h" />`
- L853: `<ClInclude Include="Source\Network\NetResyncSelfTest.h" />`
- L1435: `<ClCompile Include="Source\Network\NetResyncState.cpp" />`
- L1436: `<ClCompile Include="Source\Network\NetResyncSelfTest.cpp" />`

`RTEA.vcxproj.filters`
- L535–L537: `NetResyncState.h` + `<Filter>Network</Filter>` + close
- L538–L540: `NetResyncSelfTest.h` + `<Filter>Network</Filter>` + close
- L892–L894: `NetResyncState.cpp` + `<Filter>Network</Filter>` + close
- L895–L897: `NetResyncSelfTest.cpp` + `<Filter>Network</Filter>` + close

`Source/Network/meson.build`
- L12: `'NetResyncState.cpp',`
- L13: `'NetResyncSelfTest.cpp',`

### 3. Restore complete input, drop-map ownership and exact-target priming

`RTEA.vcxproj`
- L854: `<ClInclude Include="Source\Network\NetResyncRuntimeSelfTest.h" />`
- L1437: `<ClCompile Include="Source\Network\NetResyncRuntimeSelfTest.cpp" />`

`RTEA.vcxproj.filters`
- L541–L543: `NetResyncRuntimeSelfTest.h` + Filter + close
- L898–L900: `NetResyncRuntimeSelfTest.cpp` + Filter + close

`Source/Network/meson.build`
- L14: `'NetResyncRuntimeSelfTest.cpp',`

### 7. Add A7 journal helpers for recovery-arm traces

`RTEA.vcxproj`
- L879: `<ClInclude Include="Source\Network\NetA7Journal.h" />`
- L1461: `<ClCompile Include="Source\Network\NetA7Journal.cpp" />`

`RTEA.vcxproj.filters`
- L460–L462: `NetA7Journal.h` + Filter + close
- L817–L819: `NetA7Journal.cpp` + Filter + close

`Source/Network/meson.build`
- L7: `'NetA7Journal.cpp',`

### 8. Give the host a live F6 panel and codec-17 seat snapshots

`RTEA.vcxproj`
- L882: `<ClInclude Include="Source\CI\NetModerationGUIProbe.h" />`
- L1016: `<ClInclude Include="Source\Menus\NetModerationGUI.h" />`
- L1464: `<ClCompile Include="Source\CI\NetModerationGUIProbe.cpp" />`
- L1598: `<ClCompile Include="Source\Menus\NetModerationGUI.cpp" />`

`RTEA.vcxproj.filters`
- L427–L429: `NetModerationGUI.h` + `<Filter>Menus</Filter>` + close
- L430–L432: `NetModerationGUIProbe.h` + `<Filter>System</Filter>` + close
- L1267–L1269: `NetModerationGUI.cpp` + `<Filter>Menus</Filter>` + close
- L1270–L1272: `NetModerationGUIProbe.cpp` + `<Filter>System</Filter>` + close

`Source/Network/meson.build`: no lines (moderation units live in `Source/CI/meson.build` and `Source/Menus/meson.build`, whole-file in this commit).

## File to commit

Whole files follow `scratch/plan.json` except the three build files, which appear in commits 2, 3, 7, and 8 as above. Judgement calls for the other files are unchanged from attempt 1 (LuaMan three concerns; lockstep+ownership+priming; NetLobbyProtocol with the envelope; GameActivity reclaim checks with peer-local; Controller F6 gate with peer-local; Main/NetMatchService A7 hooks with B2).

## Verification

### 1. Before

Command: `python ...\preflight2.py`

Evidence: `scratch/preflight-attempt2.json`, `scratch/hashes-before-attempt2.json`.

- HEAD `60cb6981462d30400d372ca778d440c82a5bcbe3`.
- Every path in attempt-1 `hashes-before.json` matched. `changed_vs_attempt1=[]`.
- Tag `w1-split-attempt1` = `aaf7057e5dfef135a9b9f09fbc6831672e091895`.

### 2. Positive20 after reverse-apply

Commands: `git apply -R --check` / `git apply -R` on `handoff-20260910\post-positive20.patch`; `python ...\verify_working_set.py working-set-verify-before-commits.json`.

- Manifest sha256 `29938cd8ba995ab728c3710d66644f4c01659bb29b4a66954bd7fa78ed14c1ad`.
- 78 working-set files match. `NetRecoveryJournal.h/.cpp` absent.
- `scratch/working-set-verify-before-commits.json`.

Mac full-tree leftover (unchanged from attempt 1; does not affect the 78 files): 91 paths in `positive20.manifest.json` differ from this checkout, plus missing `AbortCode.txt`. 70 of those 91 are CRLF-only. The other 21 are larger in the Mac manifest than the Windows `60cb698146` blob by 12 to 2262 bytes (`scratch/mac-vs-head-21.json`): e.g. `Actor.cpp` 96437 vs 94175, `PrimitiveMan.h` 31628 vs 31616, `tools/contracts/AUDIT.md` 24445 vs 24443. Neither CRLF→LF nor LF→CRLF of the HEAD blob reproduces the Mac hash. None of the 21 appear as a `diff --git` path in `b2-mac-positive20/windows-wip.patch`. The Mac positive20 base checkout differs from Windows `60cb698146` in 21 files by content of unknown provenance; OPEN, to be diffed on the Mac; it does not affect the 78-file working set, which matches byte-for-byte.

### 3. After commits, before WIP restore

- `git diff --stat HEAD`: only the two `*.lib` files (`scratch/diff-stat-HEAD-attempt2.txt`).
- `git status --porcelain`: those two libs (`scratch/status-after-commits-attempt2.txt`).
- 78 working-set files still match (`scratch/working-set-verify-after-commits.json`).
- `60cb698146..HEAD` is 78 files / +9449/−770 (`scratch/split-stat-attempt2.txt`).

### 4. After WIP restore

Commands: `git apply --check` / `git apply` on `post-positive20.patch`; `python ...\hash_after.py`; `python ...\compare_before_after_hashes.py`.

- Handoff manifest: `mismatch_count=0` (`scratch/manifest-compare-after.txt`).
- `hashes-before.json` vs `hashes-after.json`: no added/removed/changed paths, including both libs (`scratch/hashes-before-vs-after.json`).

## Attribution

Command: `git log --format=%B 60cb698146..HEAD`

Evidence: `scratch/attribution-scan-attempt2.txt`.

Pattern `co-authored|claude|codex|grok|chatgpt|openai|anthropic|cursor|generated`: no matching lines.

## Commands

Logged in `commands2.log`.

1. `python ...\preflight2.py`
2. `git apply -R --check` / `git apply -R` `post-positive20.patch`
3. `python ...\verify_working_set.py` ; `python ...\remap_build_lines.py`
4. `python ...\execute_commits2.py` — `git add -- <regular files>`; build files via `git hash-object -w` + `git update-index --cacheinfo 100644,<blob>,<path>`; `git commit -F scratch/commit-msgs/NN.txt`
5. `git diff --stat HEAD` ; `git diff --stat 60cb698146 HEAD` ; working-set verify ; attribution scan
6. `git apply --check` / `git apply` `post-positive20.patch` ; hash compare

Not done: push. Not done: engine launch or build. Tag `w1-split-attempt1` not moved.

## Lead note (2026-09-11): the 21 content mismatches are CLOSED

W6 (grok-workers/w6-mac-base-provenance/REPORT.md) fetched the 21 Mac files: every extra byte is a CR (mixed CRLF+LF on the Mac, LF-only Windows blobs); LF-normalised they equal git show 60cb698146:<path> byte for byte (lead spot-checked Actor.cpp: 2262 CRs = the 2262-byte delta). The Mac positive20 tree therefore carries the same code as Windows 60cb698146 plus the 78-file working set for all 9057 files. The Mac 'positive' repo itself is a detached checkout of 316e963757 (274 commits behind) with 339 dirty paths; future Mac work should start from a clean checkout of origin/stage2/takeover-next (0e2aa3812f).
