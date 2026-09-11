# W4 breadth-harness repairs

## Pin-guard analysis

See `pin-guard-analysis.md` and `pin-measure.json` (command: `python grok-workers/w4-breadth-harness/setup_and_measure.py`).

`Pin.verify()` compares: build.json bytes; `D:\Projects\p4b-interp-validation\Cortex Command.exe` SHA-256 vs `bb3cf264b4e7304f45486440998e4b63e37d5d46d0d119caa04f76c2fc378ffb` (match); `git rev-parse HEAD` vs pinned `c8f8188ae0b4f4524f83ee5cdaebf4ef0ec3e49e` (working `9751a90e292eb300013c1d6052f3df7bbe228869`); 9043 export blobs in `combined-source-41.manifest.json` including `tools/compare_snapshots.py` pinned `dfa1cdfe…` vs working `5843330c…`; `git ls-files Source` membership; six font samples vs `git show {pin}:{name}`; `artifacts=True` at `main()`; `driver_inputs()` after plan.

`binary_matches_source` is a named interp check from `stage2_p4/harness_common.ps1` `Get-BinaryMatchesSource` (exe mtime vs Source inputs). The wrapper does not compute that string.

HEAD `c8f8188ae0` → `9751a90e29` was `tools/compare_snapshots.py` + new `tools/test_snapshot_inventory_roles.py` (not in export). That state would fail `Pin.verify`. The lead later detached the approved tree at `c8f8188ae0`. Preflight `Pin.verify(artifacts=True)` then passed (`pin-verify-rerun.txt`, `2026-09-11T05:11:03Z`).

## Diff summary

`harness.diff` (command: `python grok-workers/w4-breadth-harness/write_diff.py`).

- A. `run_breadth.py` `child()`: `sys.path.insert(0, str(STAGE))` before `heal()`. `D:\Projects\stage2_p4\recovery_e2e.py` exists. `recovery_expanded_mod.py` not edited.
- B. `compat_lines_match`: every line exact except `spawn_child` which must match `^PRINT: \[deferral\] case=spawn_child ok=1 uid=\d+$`. Oracle `COMPAT_LINES_ORACLE`. Comment cites `report.md:1531-1534` and `:1574`.
- C. `compat_review_extra.lua:97` `HasAnySounds()` → `HasAnySounds(true)`. Fixtures dir search: that is the only no-arg SoundSet call. Left as SoundContainer no-arg: `:101`, `:129`, `:139`, `:210`, `:228`. Extra pin updated after s41b4 (see Reruns).
- D. `fake_lag_auto_delay_failures`: parse `[net-match] auto input delay: peer 2 rtt (\d+)ms -> (\d+) frames`. Formula `ceil(rtt/(1000/30))+1` (`NetMatchRunner.cpp:80-86`). Band `[2*lag, 2*lag+15]`. Both peers `peer_input_delays == {"1":1,"2":frames}`. `D:\mx\s41b3\j28\fl\fl200\client\stdout.log` has no equivalent line (host-only in `NetMatchRunner.cpp:79`).
- `--cases` filter via `select_jobs` after the 81-inventory check.

## Tests

| run | command | result | path |
|---|---|---|---|
| before | `python -B -m unittest test_run_breadth.py test_family_breadth.py -v` cwd `takeover-20260909` | 47 tests, 0.294s, OK | `tests-before.txt` |
| after | `python -B -m unittest test_run_breadth.py test_family_breadth.py test_run_breadth_repairs.py -v` | 66 tests, 0.325s, OK | `tests-after.txt` |
| against-pre | same repairs file with `BREADTH_UNDER_TEST=pre\run_breadth.py` `FIXTURE_UNDER_TEST=pre\compat_review_extra.lua` | 19 tests, 18 FAIL, 1 OK | `tests-against-pre.txt` |

Against-pre FAIL names: `test_cases_filter_selects_named_jobs_only`, `test_unknown_case_is_rejected`, `test_any_other_line_difference_fails`, `test_extra_line_fails`, `test_identical_lines_pass`, `test_reordered_lines_fail`, `test_spawn_child_line_missing_fails`, `test_spawn_child_ok_zero_fails`, `test_uid_only_difference_passes`, `test_soundset_hasanysounds_passes_bool`, `test_201_to_8_passes`, `test_400_to_13_passes`, `test_400_to_14_fails_formula`, `test_401_to_14_passes`, `test_430_to_14_fails_band`, `test_line_missing_fails`, `test_peer_input_delays_disagree_fails`, `test_child_heal_imports_recovery_e2e` (`imported=False` / `ModuleNotFoundError`). Against-pre OK: `test_import_recovery_e2e_with_stage_on_path`.

## Reruns

Preflight: `python grok-workers/w4-breadth-harness/run_pin_verify.py` → `pin-verify-rerun.txt` pass, head `c8f8188ae0b4f4524f83ee5cdaebf4ef0ec3e49e`, exe `bb3cf264b4e7304f45486440998e4b63e37d5d46d0d119caa04f76c2fc378ffb`, `source_files` 9043.

Command: `python -B D:\Projects\reviews\takeover-20260909\run_breadth.py --source 41 --build-manifest D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json --out D:\mx\s41b4 --cases heal compat_deferral_source22 compat_deferral_approved compat_extra_source22 compat_extra_approved fl200 fl100` (wrapper stdout `s41b4-wrapper.out`). Wrapper `execution_pass` false because `--cases` expected_jobs is 7 != 81 (`breadth.json` `validation_errors`). Size without reparse points: 22.0 MB (`size_no_reparse.py D:\mx\s41b4`). Evidence dump: `s41b4-collect.json`.

| case | verdict | key measured values | paths |
|---|---|---|---|
| fl100 | `passed=true` `exit=0` `failures=[]` | host `rtt 202ms -> 8 frames`; both peers `peer_input_delays={"1":1,"2":8}`; client has no auto-delay line | `D:\mx\s41b4\breadth.json`; `logs\fl100.log`; `j27\fl\fl100\host\stdout.log:5`; `j27\fl\fl100\host_report.json`; `j27\fl\fl100\client_report.json` |
| fl200 | `passed=true` `exit=0` `failures=[]` | host `rtt 400ms -> 13 frames`; both peers `peer_input_delays={"1":1,"2":13}`; client has no auto-delay line | `D:\mx\s41b4\breadth.json`; `logs\fl200.log`; `j28\fl\fl200\host\stdout.log:5`; `j28\fl\fl200\host_report.json`; `j28\fl\fl200\client_report.json` |
| heal | `passed=false` `exit=0` `failures=["missing required check prediction_executed x1"]` | engine launched (`logs\heal.log`: `resync_heal PASS []`); import defect gone | `D:\mx\s41b4\breadth.json`; `logs\heal.log`; `j73\result.json` |
| compat_deferral_source22 | `passed=true` `exit=0` `failures=[]` | uid exclusion accepted | `D:\mx\s41b4\breadth.json`; `logs\compat_deferral_source22.log`; `j74\summary.json` |
| compat_deferral_approved | `passed=true` `exit=0` `failures=[]` | uid exclusion accepted | `D:\mx\s41b4\breadth.json`; `logs\compat_deferral_approved.log`; `j75\summary.json` |
| compat_extra_source22 | `passed=false` `exit=0` `failures=["compat fixture changed","compat named case results differ from Source22","compat output differs from Source22 (spawn_child uid value excluded, run-to-run counter)"]` | 19 PRINT lines including `shared_soundset_structural before=1 removed=0 any=1`; no `ERROR:`; fixture `e92fa5fb…` | `D:\mx\s41b4\j80\summary.json`; `logs\compat_extra_source22.log` |
| compat_extra_approved | `passed=false` `exit=0` `failures=` same three strings | PRINT-line list identical to Source22 (19/19, diffs=[]); no `ERROR:` | `D:\mx\s41b4\j81\summary.json`; `logs\compat_extra_approved.log` |

compat_extra pin: Source22 and approved PRINT lists identical and neither console had `ERROR:`. Pin updated from Source22 `D:\mx\s41b4\j80\summary.json` (approved capture `D:\mx\s41b4\j81\summary.json`). `BASELINE["compat"]["extra"]` now `fixture_sha256=e92fa5fb5095b4cec374a6f7ef69c3559edb7028e6ed2fbc00a2fe214eccfaaf` plus `shared_soundset_structural`. Guards after pin: 66 OK `tests-after-pin.txt`. `harness.diff` regenerated.

extra2 command: same wrapper, `--out D:\mx\s41b4-extra2 --cases compat_extra_source22 compat_extra_approved` (`s41b4-extra2-wrapper.out`). Per-case: both `passed=true` `exit=0` `failures=[]` (`D:\mx\s41b4-extra2\breadth.json`). Wrapper `execution_pass` false only for inventory 2 != 81 (`validation_errors`: `declared breadth inventory differs`, `breadth step coverage/order differs; missing, duplicate, or unexpected case`).

## Could not do

- Wrapper aggregate `execution_pass` on a `--cases` subset (inventory check still requires 81). Per-case rows above are the case verdicts.

