# tools/handtest - manual multiplayer hand-test kit

`play.ps1` launches isolated headed instances for a person at this PC;
`collect_logs.ps1` packs their logs for review. The full runbook is
`HANDTEST.md` at the repo root.

## Isolation

Each instance gets a private runtime `D:\mx\handtest\<role>-<n>\`:

- `Userdata\Settings.ini` copied from the build's template and pinned in place
  (idempotent - keys are replaced, never duplicated): windowed 1280x720, sound
  on, `DeltaTime = 0.016667`, `SkipIntro`, and the same lockstep pins the harness
  uses (`tools/feel_measure.py` `private_settings`).
- `Data` is a junction to the build's `Data` - shared read-only content,
  zero copies. Never delete the runtime root with a junction-following tool.
- `Mods`, `ScreenShots`, `Temp`, `Autosaves` are per-instance; `TEMP`/`TMP`
  point inside it; `reconnect.ticket`, `host-bans.txt`, `match-report-*.json`,
  `crash*.dmp`, `AbortCode*.txt` all land there too.
- Every launch is recorded in `kit-launch-<stamp>.json` and writes
  `console-<stamp>.out|err.log` - no shared file is overwritten between runs.
- Headed instances are windowed and placed left-to-right (host left, clients
  right, clamped to the work area).

Headed mode refuses to run when `CCCP_HEADLESS` is in the environment (an
automation shell - it would leave hidden engines). `-LossPct` is accepted only
with `-Headless`: the engine honors `CC_TEST_GNS_LOSS_PERCENT` solely under
headless+lockstep. `-FakeLagMs` works in both modes (`-net-fake-lag`).

## Self-check (workers only)

`play.ps1 -Headless` runs the same flow through `tools/isolated_launch.py`
(CCCP_HEADLESS=1, private hidden desktop) with a menu-script driving the real
lobby UI and `-net-match-ticks` bounding the match. Its runtimes live under
`D:\mx\handtest\_selfcheck\` - the collector never mixes them into a user pack.

## Evidence

`collect_logs.ps1 -Out <dir>` copies `console-*`, `run-*\`, `match-report-*`,
`kit-launch-*`, autosaves, replays, crash dumps and the effective Settings.ini
per instance, plus a `MANIFEST.txt` (build sha + dirty flag of the exe's tree,
exe sha256, argv, exit codes, per-file sizes/mtimes). `reconnect.ticket` is not
collected - a live rejoin credential. Recursive copies never follow junctions.
`-Source <dir>` packs a different root (e.g. `D:\mx\handtest\_selfcheck`).
