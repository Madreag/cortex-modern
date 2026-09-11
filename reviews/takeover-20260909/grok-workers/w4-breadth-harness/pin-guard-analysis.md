# Pin / `binary_matches_source` (wrapper)

Command: `python grok-workers/w4-breadth-harness/setup_and_measure.py` → `pin-measure.json`.

## What the wrapper itself checks

`run_breadth.py` `Pin.verify()` (`1251-1321` after this edit; same logic as the pre-fix copy). `main()` calls `pin.verify(artifacts=True)` before any job. `run_jobs` calls `pin.verify()` before and after every job. Heal/invariance `child()` sets `source_unchanged` from a post-run `pin.verify()`.

Compared objects:

1. Build manifest file bytes: `digest(grouped-build-bb3cf264/build.json)` vs the digest taken at `Pin.__init__`. Path used by Source41: `D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json`. That file has no `inputs` key.
2. Approved executable bytes: `digest(D:\Projects\p4b-interp-validation\Cortex Command.exe)` vs `build.json` `"exe_sha256"` = `bb3cf264b4e7304f45486440998e4b63e37d5d46d0d119caa04f76c2fc378ffb`. Measured: match.
3. Git HEAD: `git -C D:\Projects\p4b-interp-validation rev-parse HEAD` vs `build.json` `"head"` = `c8f8188ae0b4f4524f83ee5cdaebf4ef0ec3e49e`. Measured HEAD: `9751a90e292eb300013c1d6052f3df7bbe228869`. **Fails** with `source HEAD changed: 9751a90e292eb300013c1d6052f3df7bbe228869`.
4. Combined-source export file blobs: every path in `combined-source-41.manifest.json` `"files"` (`D:\Projects\reviews\recovery-2026-09-07\contract-audit\mac-peer-20260907\combined-source-41.manifest.json`, export head `c8f8188ae0…`, 9043 files, 570 under `Source/`). Each working-tree file SHA-256 must equal the export `sha256`. This set includes `Source/**` and also repo files such as `tools/compare_snapshots.py` (pinned `dfa1cdfee50e2a49aebe82516fbf4dd06b493a369bc3ff394497759d62f40561`). Measured working `tools/compare_snapshots.py` = `5843330c5d4be7585c99c0d713be990ff92653bc6249b9633534baabba676fb6`. **Fails** with `source files differ from compiled export: ['tools/compare_snapshots.py']`.
5. New git paths under `Source/`: `git ls-files --cached --others --exclude-standard -z Source`. Every name must be in the export, except the six Dear ImGui sample TTFs in `SOURCE_FONT_SAMPLES`, which are compared to `git show {pinned_head}:{name}`. A new `tools/` file is not covered by this Source-only walk.
6. `tools/test_snapshot_inventory_roles.py` is **not** in the export. It is not an engine-`Source/` path, so it does not by itself trip (5). It is still a HEAD-moving commit.
7. When `artifacts=True` (start of `main()`): every path/hash in `build.json` `"artifacts"` (export json/tar/patch, build log, grouped exe, grouped pdb).
8. After `plan()`: `driver_inputs()` hashes the planned driver/fixture paths plus a fixed set (`run_breadth.py`, `recovery_e2e.py`, `win32_test_runner.py`, compat fixtures, …) and the Source22 reference exe pin `4244e87e…`.

`inspect_launches` additionally requires each `launch.json` `exe_sha256` and `argv[0]` to be the approved exe (or the Source22 reference on a source22 compat rung). That is the launched blob, not the source export.

## Named check `binary_matches_source`

This string is a required interp (and some other family) check name in `BASELINE`. The wrapper does not compute it. `stage2_p4/harness_common.ps1` `Get-BinaryMatchesSource` is an mtime heuristic: exe newer than every build input, or built by this run. A tools-only commit does not change `Source/` mtimes relative to the unchanged exe, so that named check can still pass while `Pin.verify()` fails.

## HEAD `c8f8188ae0` → `9751a90e29`

`git show --stat 9751a90e29`: `tools/compare_snapshots.py` (+10/−2), `tools/test_snapshot_inventory_roles.py` (new, +149). Exe hash unchanged. `Pin.verify()` fails on (3) and (4). Guard and pin were not edited.

**Step 6 not started.**
