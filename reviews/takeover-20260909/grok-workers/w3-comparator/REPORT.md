# W3 comparator repair verification

Input tree: `D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910`.
Write root: `D:\Projects\reviews\takeover-20260909\grok-workers\w3-comparator`.
Main tree was read-only. `git apply` without `--check` was not run. No engine launch.

## 1. Input inventory

Listed by `Get-ChildItem ... -Recurse -File` (command recorded in this report's command log).

| Path under the input tree | Role |
|---|---|
| `review.md` | Isolated repair notes |
| `independent-review.md` | Later independent-check notes |
| `verification.json` | First-pass commands, hashes, embedded 8-line source diff |
| `role-evidence.json` | Host/client UIDs and inventory paths for the retained pair |
| `verify_independently.py` | Re-hash + re-run script used for `independent-check/` |
| `control\compare_snapshots.py` | Frozen unrepaired comparator |
| `control\snapshot_runtime.py` | Frozen runtime (hash-equal to repaired) |
| `control\test_compare_snapshots.py` | Frozen existing 53 comparator tests |
| `control\test_snapshot_inventory_roles.py` | Detecting tests (8 methods) |
| `control\tests.log` | Prior control unittest log (8 tests only) |
| `control\retained-pair.log` | Prior control pair stdout |
| `control\retained-pair.json` | Prior control pair `--report` |
| `repaired\compare_snapshots.py` | Frozen repaired comparator |
| `repaired\snapshot_runtime.py` | Frozen runtime (hash-equal to control) |
| `repaired\test_compare_snapshots.py` | Same detecting-suite companion as control |
| `repaired\test_snapshot_inventory_roles.py` | Same detecting tests as control |
| `repaired\tests.log` | Prior repaired unittest log (61 tests) |
| `repaired\retained-pair.log` | Prior repaired pair stdout |
| `repaired\retained-pair.json` | Prior repaired pair `--report` |
| `independent-check\verification.json` | Independent re-run record |
| `independent-check\control-tests.log` | Independent control unittest log |
| `independent-check\repaired-tests.log` | Independent repaired unittest log |
| `independent-check\control-pair.log` | Independent control pair stdout |
| `independent-check\repaired-pair.log` | Independent repaired pair stdout |
| `independent-check\control-pair.json` | Independent control pair `--report` |
| `independent-check\repaired-pair.json` | Independent repaired pair `--report` |

No `.ccsave` files live in that tree. The retained `mp_snapshot_p5` pair is referenced by `verification.json` as:

- `D:\mx\s41r2\mp_snapshot_p5\fresh\e2e\snapshot_p5\p5snap_p1.ccsave`
- `D:\mx\s41r2\mp_snapshot_p5\fresh\e2e\snapshot_p5\p5snap_p2.ccsave`

Those two files were hashed only (not copied, moved, or rewritten). Hashes in `retained-pair-hashes.txt` and `commands.json` match `verification.json` before and after the pair runs.

## 2. Provenance

Command: `python w3-comparator\make_repair_diff.py` → `provenance-hashes.txt`.

| File | sha256 |
|---|---|
| `control\compare_snapshots.py` | `dfa1cdfee50e2a49aebe82516fbf4dd06b493a369bc3ff394497759d62f40561` |
| `repaired\compare_snapshots.py` | `5843330c5d4be7585c99c0d713be990ff92653bc6249b9633534baabba676fb6` |
| main `tools\compare_snapshots.py` | `dfa1cdfee50e2a49aebe82516fbf4dd06b493a369bc3ff394497759d62f40561` |

`control == main` byte-for-byte (`control_equals_main True`). `git log` on the file was not required.

Additional hashes (`provenance-hashes.txt`):

- `control`/`repaired` `test_snapshot_inventory_roles.py`: `80ac96f9843a3e7368957e5f97d914b6780be04f795fd98c89f0e102bc8c1d82` (equal)
- `control`/`repaired` `test_compare_snapshots.py`: `b827f18e6aa077e27ee34800722c1d2a3ddd1b135c1977f91b035efc2da54340` (equal)
- `control`/`repaired` `snapshot_runtime.py`: `553b3f11a33a4689c73999eb65e8572c494b745c6cf53c9e615f6c8287a6cc32` (equal)
- main `tools\test_compare_snapshots.py`: `92c4c994ccd3d300b9d30ce4125a36aedc2523811db80add8b9df4471600951b`

`difflib` text diff of frozen vs main `test_compare_snapshots.py` is empty (`test_compare_snapshots-frozen-vs-main.diff` is empty). `newline-audit.txt`: frozen file is LF (38802 bytes), main file is CRLF (39417 bytes), 615 lines each. The patch does not touch that file.

Main HEAD during this work: `c8f8188ae0b4f4524f83ee5cdaebf4ef0ec3e49e`.

## 3. Hunk summary (`repair.diff`)

One hunk, `inventory_reference_roles`, `@@ -550,9 +550,15 @@`:

1. Version gate changes from `!= "Activity1"` to `not in ("Activity1", "Activity2", "Activity3")` so Activity2/3 get the same player-link projection as Activity1.
2. Local actor selection changes from `state["actor_links"][0][0]` (brain) to `links[0][1]` (controlled actor).
3. **Not one of those two concerns:** the hunk also requires `actor_links` to be a list of exactly four slots, each a list of exactly three non-negative `int` (not `bool`), and raises `ValueError("invalid activity actor-link layout")` otherwise. That is new rejection, not a version or column-index fix.

No other files or functions differ between control and repaired `compare_snapshots.py`. The existing `if local_actor not in roles: raise ValueError("the local controlled actor is missing from the snapshot world")` is unchanged.

## 4. Detecting tests

Same command on both copies (`commands.json`):

```
"C:\Program Files\Python314\python.exe" -B -m unittest test_snapshot_inventory_roles test_compare_snapshots -v
```

| Copy | cwd | log | returncode | unittest footer |
|---|---|---|---|---|
| repaired | `...\snapshot-inventory-roles-20260910\repaired` | `tests-repaired.txt` | 0 | `Ran 61 tests in 0.686s` / `OK` |
| control | `...\snapshot-inventory-roles-20260910\control` | `tests-control.txt` | 1 | `Ran 61 tests in 0.616s` / `FAILED (failures=32, errors=4)` |

The 53 `test_compare_snapshots.*` methods are `ok` on both copies.

On control, 4 of the 8 `test_snapshot_inventory_roles` methods fail (verbatim names from `tests-control.txt`):

- `test_supported_versions_map_controlled_actor_and_exact_owned_path` — 3 ERROR (`KeyError: 10` for Activity1/2/3)
- `test_local_pairs_match_but_wrong_owner_item_and_aliases_do_not` — 3 FAIL (`correct_pair` for Activity1/2/3)
- `test_malformed_actor_link_layout_is_rejected` — 1 ERROR (`Activity1`, `links=None`, `TypeError`) and 26 FAIL (`ValueError` not raised)
- `test_no_controlled_actor_does_not_fall_back_to_brain` — 3 FAIL (Activity1 maps brain 100; Activity2/3 do not raise)

The other 4 role methods are `ok` on control: `test_game_activity_wrappers_keep_versioned_bindings`, `test_projection_stays_local_and_preserves_other_fields`, `test_truncated_or_mislabeled_native_records_are_rejected`, `test_unsupported_versions_are_not_projected`.

The 61-test suite does not all pass on control.

## 5. Retained pair

Pair hashes (`retained-pair-hashes.txt`; unchanged after the runs, `commands.json` `pair_unchanged: true`):

- p1 `d0513a568a0e22870336a84d6d4a4d1dce06119aa67d40a0de5e06b72716ba4a`
- p2 `05dca2684c355ac9d7208a8594f41bbfbd2bc7022411f6ae5bb91c1270b2f004`

Command shape (no `--full`, no threshold flags):

```
python -B <copy>\compare_snapshots.py <p1> <p2> --report <w3-comparator>\retained-<copy>.json
```

Stdout verdict lines, quoted:

- control (`retained-control.log`, returncode 1): `FAIL: Save.ini Activity property 'SpecialBehaviour_RuntimeCheckpoint' differs across peers (sim state)`
- repaired (`retained-repaired.log`, returncode 0): `PASS: shared snapshot state matches (5 entries; 5 Lua VMs; activity local differences: 2)`

`--report` fields: control `passed: false` with that one failure (`retained-control.json` lines 3311–3314); repaired `passed: true`, `failures: []`, `activity_local_differences: 2` (`retained-repaired.json` lines 3315–3317).

## 6. Mask / tolerance audit

Grep of `repair.diff` for `ignore|skip|exclude|tolerance|allow` (case-insensitive): no matches. Output: `mask-grep.txt` (`NO_MATCHES`).

The new layout check rejects malformed rows; it does not add an ignore/skip/exclude path or loosen a numeric tolerance.

## 7. Where tool tests live on main

`Get-ChildItem D:\Projects\p4b-interp-validation\tools -Recurse -Filter test_*.py` lists sibling files under `tools\` (`test_compare_snapshots.py`, `test_compare_sim_traces.py`, …). There is no `tools\tests\` directory.

Convention in those files: `if __package__: from . import <tool> else: import <tool>`, plus `unittest.main()`. No repo doc or CI snippet naming a runner was found by searching `*.md`/`*.yml`/`*.ps1`/`*.py` for `python -m unittest` / `pytest`. The isolated verification and this run used `python -m unittest <modules> -v` with cwd = the directory that contains both the tool and `test_*.py`.

The detecting tests were therefore placed at `tools/test_snapshot_inventory_roles.py` in the patch, not `tools/tests/test_compare_snapshots.py`.

## 8. Patch and `apply --check`

Scratch repo: `w3-comparator\scratch-repo\` (`git init` only there). Baseline commit = main's `tools/compare_snapshots.py`. Second commit = repaired file + `tools/test_snapshot_inventory_roles.py`. Message: `commit-message.txt`. `git format-patch -1 --stdout` → `compare_snapshots-repair.patch`. Files in the patch: `tools/compare_snapshots.py` (10-line hunk) and new `tools/test_snapshot_inventory_roles.py` (149 lines). No `Co-Authored-By` / `Claude-Session` in `scratch-git.log` or the patch.

Command: `git -C D:\Projects\p4b-interp-validation apply --check <patch>` → `apply-check.txt` `EXIT=0`. After the check: HEAD still `c8f8188ae0b4f4524f83ee5cdaebf4ef0ec3e49e`; `git status --porcelain` empty for the two paths.

## 9. Commands and output paths

| Command / script | Output |
|---|---|
| `make_repair_diff.py` | `provenance-hashes.txt`, `repair.diff`, `mask-grep.txt` |
| frozen vs main test text diff | `test_compare_snapshots-frozen-vs-main.diff` (empty) |
| `check_newlines.py` | `newline-audit.txt` |
| `run_w3_verify.py` | `tests-repaired.txt`, `tests-control.txt`, `retained-control.log`, `retained-repaired.log`, `retained-control.json`, `retained-repaired.json`, `retained-pair-hashes.txt`, `commands.json` |
| `build_patch.py` | `scratch-repo\`, `scratch-git.log`, `compare_snapshots-repair.patch` |
| `git apply --check` | `apply-check.txt` |
