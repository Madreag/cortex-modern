# W6 Mac base-file provenance

Evidence root: `D:\Projects\reviews\takeover-20260909\grok-workers\w6-mac-base-provenance\`.
Mac repo: `/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive`.
Windows blobs: `git -C D:\Projects\cccp show 60cb698146:<path>` (read-only). No writes on the Mac. No writes in any repository.

## 1. Mac git state

Commands (read-only over `ssh Erol-Mac`, script `mac_collect.sh` piped to `zsh`; stdout `mac-git-state/mac_collect_stdout.txt`):

- `git -C <repo> rev-parse HEAD` → `316e96375702eff5b0118714dc4762a0ee6eebd7`
- `git -C <repo> status -sb` → `## HEAD (no branch)`
- `git -C <repo> log --oneline -5` →
  - `316e96375 feat(snapshot): encode portable runtime values and full controller state`
  - `d435c66d3 fix(snapshot): retain configuration of derived native object classes`
  - `99e4cc0b9 fix(snapshot): preserve nested native identities during writes`
  - `8b10e7ee3 fix(craft): set the lateral control speed field`
  - `b667f492e fix(turret): handle empty mounts and release owned devices`
- `git -C <repo> remote -v` → `origin  git@github.com:Madreag/cortex-modern.git` (fetch and push)
- `git -C <repo> config core.autocrlf` → unset (`CONFIG_AUTOCRLF_EXIT:1`)
- `git -C <repo> diff --stat` → `268 files changed, 61678 insertions(+), 27408 deletions(-)`
- `git -C <repo> diff --cached --stat` → empty
- `git -C <repo> status --porcelain` → 339 lines: 268 ` M`, 71 `??` (`mac-git-state/status-counts.json`)

Follow-up (`mac_more.sh` → `mac-git-state/mac_more_stdout.txt`):

- `git -C <repo> log -1 --format=fuller HEAD` → AuthorDate/CommitDate Mon Sep 7 13:27:27 2026 -0700, subject `feat(snapshot): encode portable runtime values and full controller state`
- `git -C <repo> cat-file -t 60cb698146` → `fatal: Not a valid object name 60cb698146` (exit 128)
- All 21 listed files: ` M` (working tree dirty, index blob equals `HEAD:<path>`)

**Mac HEAD is not `60cb698146`.** It is detached `316e963757`.

**All 21 files are listed as modified in the Mac's own `git status`.** They are local uncommitted working-tree edits relative to Mac HEAD `316e963757`. They are not clean committed blobs of a different commit. Index matches HEAD for each of the 21 (`HEAD_BLOB` == `INDEX_BLOB` in `mac_more_stdout.txt`).

Windows object store (`git -C D:\Projects\cccp`, `mac-git-state/win-relationship.txt`):

- `316e963757` is an ancestor of `60cb698146`
- `merge-base(316e963757, 60cb698146) = 316e963757`
- `rev-list --count 316e963757..60cb698146` = 274
- `rev-list --count 60cb698146..316e963757` = 0
- `60cb698146` subject: `Compose moderation with the current lockstep and reconnect work`

Mac HEAD blobs for the 21 paths equal Windows `316e963757:<path>` (`mac-git-state/win-blob-compare.json`). Six of those blobs also equal `60cb698146:<path>`: `GAScripted.h`, `Gib.cpp`, `GlobalScript.cpp`, `GlobalScript.h`, `tools/contracts/AUDIT.md`, `tools/fixtures/mod_checkpoint.lua`. The other 15 differ between `316e963757` and `60cb698146`.

## 2. Per-file Mac diffs and Windows copies

- Mac WT vs Mac HEAD: `git -C <repo> diff -- <path>` saved under `mac-diffs/` (combined capture `mac-git-state/mac_diffs_all.txt`, 919913 bytes).
- Mac working-tree copies: `scp` → `mac-files/<path>`
- Windows `60cb698146` blobs: `win-blobs-60cb698146/<path>`
- Win-vs-Mac: `C:\Program Files\Git\usr\bin\diff.exe -u --strip-trailing-cr` → `win-vs-mac-diffs/` (all 21 files 0 bytes, exit 0). Note: `win-vs-mac-diffs\_EMPTY.txt`

Byte deltas Mac-minus-`60cb698146` match the brief exactly (`win-vs-mac-summary.json`).

## 3. What the extra Mac bytes are

Command: `python analyze_endings.py` → `ending-analysis.json`.

For every one of the 21 files:

- `byte_delta == cr_delta` (every extra Mac byte is a CR)
- after `CRLF`/`CR` → `LF`, Mac working-tree bytes equal `git show 60cb698146:<path>`
- `diff --strip-trailing-cr` exit 0
- Windows blob is LF-only (`crlf=0`)
- Mac working tree is mixed CRLF + LF
- wholesale `LF→CRLF` of the Windows blob does not equal the Mac file (`win_crlf_equals_mac=false`)
- wholesale `CRLF→LF` of the Windows blob does not equal the Mac file either (Windows is already LF; Mac still has leftover mixed CRs)

There is no added or removed non-CR text versus `60cb698146`. The Mac-vs-HEAD unified diffs under `mac-diffs/` are against `316e963757`, not against `60cb698146`. For the six paths where `316e963757:<path> == 60cb698146:<path>`, those Mac-vs-HEAD diffs are CR-at-EOL on existing lines.

## 4. Parked patch

Walker `find_parked.py` (skips `D:\mx`, `runtime`, `Data`, `external`, `.git`, reparse points) → `parked-search.json`.

Canonical directory: `D:\Projects\native-fidelity-work\parked-20260908-tools-contracts` (README: parked 2026-09-08 from `p4b-interp-validation/tools/contracts/`, held out of commit-plan W3). Patch file: `tools\contracts\primitive_checkpoint_pending.patch`. That patch has three `diff --git` hunks only:

- `Source/Managers/PrimitiveMan.h`
- `Source/Renderer/GraphicalPrimitive.cpp`
- `Source/Renderer/GraphicalPrimitive.h`

Other `primitive_checkpoint_pending.patch` hits are copies under `reviews\recovery-2026-09-06\expanded-mod-gates\` and `reviews\claude-review-2026-09-08\lanes\commit-plan\scratch\`.

Textual compare of parked added lines to the Mac-vs-Windows diffs (`characterize.py` → `characterization.json`): **21 / 21 UNEXPLAINED**. The Mac-vs-Windows diffs contain no added lines, so the parked hunks do not account for the hash/size gap.

Supplementary (parked added lines vs Mac-WT-vs-Mac-HEAD, not vs `60cb698146`):

| path | vs 60cb | vs Mac HEAD 316e |
|---|---|---|
| Source/Renderer/GraphicalPrimitive.cpp | UNEXPLAINED | EXPLAINED_BY_PARKED_PATCH (25/25 parked added lines present; 0 Mac-only added lines) |
| Source/Managers/PrimitiveMan.h | UNEXPLAINED | PARTIAL (12/13 parked added lines; Mac uses `PrimitiveQueuesSetAside` + `using QueuesSetAside = ...` instead of an in-class `struct QueuesSetAside`) |
| Source/Renderer/GraphicalPrimitive.h | UNEXPLAINED | PARTIAL (14/15 parked added lines; Mac adds `#include "MovableObjectReference.h"` and `MovableObjectReference m_SpriteOwner` plus `ShareSpriteBitmap`) |
| other 18 paths | UNEXPLAINED | UNEXPLAINED (patch does not touch the path) |

`60cb698146:Source/Renderer/GraphicalPrimitive.cpp` blob `d5d1e71f0e9e` matches the parked patch new-index prefix `d5d1e71f0`. `60cb698146` PrimitiveMan.h / GraphicalPrimitive.h blobs do not match the parked new-index prefixes (`5046259844c1` vs `67c2decec`, `afdb1c383e74` vs `bb615bcce`).

## 5. Per-file table (Mac WT versus `60cb698146`)

| path | Mac HEAD status | extra Mac bytes | extra CR | Mac CRLF / lone LF | vs 60cb after LF-normalize | parked vs 60cb | one line |
|---|---|---:|---:|---|---|---|---|
| Source/Activities/GAScripted.h | ` M` | 176 | 176 | 176 / 2 | equal | UNEXPLAINED | no extra code; 176 CR on mixed endings |
| Source/Entities/Actor.cpp | ` M` | 2262 | 2262 | 2262 / 28 | equal | UNEXPLAINED | no extra code; 2262 CR on mixed endings |
| Source/Entities/Actor.h | ` M` | 1196 | 1196 | 1196 / 3 | equal | UNEXPLAINED | no extra code; 1196 CR on mixed endings |
| Source/Entities/Gib.cpp | ` M` | 82 | 82 | 82 / 31 | equal | UNEXPLAINED | no extra code; 82 CR on mixed endings |
| Source/Entities/GlobalScript.cpp | ` M` | 180 | 180 | 180 / 1 | equal | UNEXPLAINED | no extra code; 180 CR on mixed endings |
| Source/Entities/GlobalScript.h | ` M` | 118 | 118 | 118 / 2 | equal | UNEXPLAINED | no extra code; 118 CR on mixed endings |
| Source/GUI/GUIBanner.cpp | ` M` | 417 | 417 | 417 / 36 | equal | UNEXPLAINED | no extra code; 417 CR on mixed endings |
| Source/GUI/GUIBanner.h | ` M` | 228 | 228 | 228 / 8 | equal | UNEXPLAINED | no extra code; 228 CR on mixed endings |
| Source/GUI/GUIFont.h | ` M` | 109 | 109 | 109 / 3 | equal | UNEXPLAINED | no extra code; 109 CR on mixed endings |
| Source/GUI/GUIInput.cpp | ` M` | 186 | 186 | 186 / 31 | equal | UNEXPLAINED | no extra code; 186 CR on mixed endings |
| Source/Managers/PostProcessMan.cpp | ` M` | 443 | 443 | 443 / 181 | equal | UNEXPLAINED | no extra code; 443 CR on mixed endings |
| Source/Managers/PostProcessMan.h | ` M` | 245 | 245 | 245 / 8 | equal | UNEXPLAINED | no extra code; 245 CR on mixed endings |
| Source/Managers/PrimitiveMan.h | ` M` | 12 | 12 | 12 / 474 | equal | UNEXPLAINED | no extra code; 12 CR on mixed endings |
| Source/Menus/BuyMenuGUI.h | ` M` | 551 | 551 | 551 / 2 | equal | UNEXPLAINED | no extra code; 551 CR on mixed endings |
| Source/Menus/InventoryMenuGUI.h | ` M` | 422 | 422 | 422 / 2 | equal | UNEXPLAINED | no extra code; 422 CR on mixed endings |
| Source/Renderer/GraphicalPrimitive.cpp | ` M` | 27 | 27 | 27 / 271 | equal | UNEXPLAINED | no extra code; 27 CR on mixed endings |
| Source/Renderer/GraphicalPrimitive.h | ` M` | 14 | 14 | 14 / 685 | equal | UNEXPLAINED | no extra code; 14 CR on mixed endings |
| Source/System/ContractAudit.h | ` M` | 1617 | 1617 | 1617 / 406 | equal | UNEXPLAINED | no extra code; 1617 CR on mixed endings |
| tools/contracts/AUDIT.md | ` M` | 2 | 2 | 2 / 203 | equal | UNEXPLAINED | no extra code; 2 CR on mixed endings |
| tools/fixtures/mod_checkpoint.lua | ` M` | 447 | 447 | 447 / 123 | equal | UNEXPLAINED | no extra code; 447 CR on mixed endings |
| tools/run_restoration_tests.py | ` M` | 134 | 134 | 134 / 29 | equal | UNEXPLAINED | no extra code; 134 CR on mixed endings |

## 6. Other Mac Codex trees

Command: `ls -la /Users/erol/Documents/Codex` and `find ... -maxdepth 3 -name .git` in `mac_collect.sh`.

Top-level entries (none of the dated/smoke folders is a git root):

- `2026-05-01` — not git
- `2026-08-23` — not git
- `2026-08-26` — not git
- `2026-09-05` — not git
- `2026-09-07` — not git
- `claude-ssh-smoke-646592fa` — not git
- `cortex-b2-review-20260909` — not git (parent of the two repos)
- `mac-family-lifetime-20260909-2106` — not git

Nested git repos (`mac_collect_stdout.txt` `NESTED_GIT_UNDER_CODEX` plus `mac_more_stdout.txt` `CONTROL_REPO`):

| path | HEAD | branch | `status --porcelain \| wc -l` |
|---|---|---|---|
| `/Users/erol/Documents/Codex/cortex-b2-review-20260909/positive` | `316e96375702eff5b0118714dc4762a0ee6eebd7` | detached `HEAD` | 339 |
| `/Users/erol/Documents/Codex/cortex-b2-review-20260909/control` | `316e96375702eff5b0118714dc4762a0ee6eebd7` | detached `HEAD` | 316 |

`find` reported only those two `.git` paths under `/Users/erol/Documents/Codex`.

The Mac positive tree also has untracked `tools/contracts/primitive_checkpoint_pending.patch` and `tools/contracts/primitive_pending/` (`mac_collect_stdout.txt` lines 326–327).

## 7. What the Mac positive20 tree is relative to Windows `60cb698146`

The Mac positive checkout is a detached HEAD at `316e963757` (7 Sep 2026 snapshot commit), 274 commits behind `60cb698146` on the Windows object store, and the Mac repo does not contain object `60cb698146`. Its working tree is dirty: 268 modified files and 71 untracked paths, including all 21 listed files (` M`, unstaged). After stripping carriage returns, each of those 21 working-tree files is byte-identical to `git show 60cb698146:<path>`; the recorded size/hash gap is mixed CRLF on a subset of lines (Windows blobs are LF-only), which is why a uniform CRLF↔LF conversion of the Windows blob does not reproduce the Mac hash. The parked primitive-checkpoint patch does not explain that gap (21/21 UNEXPLAINED versus `60cb698146`).
