# Tools

Everything here supports building confidence in the engine: two suite runners, the drivers that play the game through
an isolated runner, the multi-machine acceptance harness, and a few utilities. Nothing in `tools/` is shipped with the
game.

## What you need

- Python 3.11 or newer, with `pillow` and `cryptography` (`python -m pip install pillow cryptography`). The suites were
  last run with Python 3.14, Pillow 12.2 and cryptography 46.0.
- A built engine at the repository root (`Cortex Command.exe` on Windows; on macOS and Linux the binary named by
  `CCCP_TEST_BINARY`, else `build-gns/CortexCommand`). See [Building](../README.md#building).
- For video scenes: `ffmpeg` and `ffprobe` on `PATH`.
- Git, for the drivers that record which commit they ran.

## The two suite runners

```
python tools/run_selftests.py --repo . --out <scratch dir> --timeout 300
python tools/run_tools_suites.py --repo .
```

`run_selftests.py` launches the engine once per row of `SELFTESTS` (its built-in `-…-selftest` modes) and scores each
from its own PASS and FAIL lines. `run_tools_suites.py` runs the tools' own tests, the ones that read the tree and never
start the game. Rows that need a Windows-only facility or a tool kept outside the repository report NOT APPLICABLE with
the reason.

## One scene

```
python tools/e2e_video.py --repo . --out <scratch dir>/mp-host-join --scenario mp-host-join
```

A scene is one play-through described by `tools/e2e/<name>.json`: the peers, their menu and input scripts, and what the
probes must read on screen. The driver records each peer on a private hidden desktop and leaves a video, a contact sheet
and `review.json`. The JSON records the peers, input scripts and assertions for the scene.

## How the game is started

Every driver starts the engine through `run_sim_test.make_run`, which stages a private writable runtime and hands the
process to `win32_test_runner.py` (Windows: a private hidden desktop, a job object, a memory cap) or
`posix_test_runner.py`. Set `CCCP_HEADLESS=1` in the environment; nothing a driver starts may open a window on your
desktop. Never start the executable directly from a script.

## The box file

Tools that drive more than one machine read the machines' names, ssh aliases, trees, scratch roots and reservation
markers from one file outside the repository: the file `CORTEX_BOXES` names, else `~/.cortex-modern/boxes.json`.
`boxes.example.json` shows the shape with neutral names (`box-a` …) and documentation addresses; copy it there and
write your machines in. `box_facts.py` reads it. Each machine has a role (`pc`, `remote`, `mac`, `linux`, `laptop`,
`handheld`) that the tools ask for. Without a box file the single-machine drivers still run (their scratch goes under the
system temp directory and no reservation marker is honoured); the multi-machine tools stop and say how to make one.
The unit tests use the example file, never yours.

Optional operator tooling is configured outside the repository. `CCCP_INVENTORY_DIR` selects an inventory folder;
`CC_INVENTORY_DIR` selects a shipped flat copy. Without either, only this tools folder is searched.
`CCCP_FEEL_MARKER` selects a reservation marker; its default is `cccp-feel-matrix-running` in the system temp folder.
`CCCP_SCRATCH_ROOT` selects a shared scratch root. `CCCP_SP_CONTROL`, `CCCP_FAMILY_LOCK` and `CCCP_BOX_LOG` select
the single-player control, family reservation and run log. `CCCP_SESSION_SCRIPT` selects the remote task's script.
`CCCP_CROSS_BOXES` selects a cross-match manifest; without it the driver reads the configured box file.
`CCCP_CROSS_REQUIREMENTS` selects an external requirements table; without it those requirements are NOT COVERED.
`CCCP_CLOUDFLARE_TURN_CONFIG` and `CCCP_COTURN_CONFIG` select relay credential files; defaults are relative files
under `relay/`. `CCCP_VPN_COMMAND` selects an optional overlay network CLI. No overlay CLI is discovered automatically.
`ACCEPTANCE_RELAY_SCENARIO` selects an external relay scenario. `CCCP_DIRECTORY_LOG_HOST` and `CCCP_DIRECTORY_LOG_DIR`
enable optional directory-log collection; without them it reads no remote logs. `CCCP_CLOUDFLARED` and `CCCP_FFMPEG`
select tool commands; otherwise the tools use `PATH` and configured manifest directories.
Remote mod targets declare `scratch_root`. World task profiles also declare `approved_task_script`, `approved_tree`
and `approved_marker`; the profile checks compare the actual paths against these explicit policy fields.

## Layout

| Path | What it holds |
|---|---|
| `run_selftests.py`, `run_tools_suites.py` | the suite runners |
| `run_sim_test.py`, `win32_test_runner.py`, `posix_test_runner.py`, `isolated_launch.py` | starting the engine in isolation |
| `test_*.py` | unit tests (run by `run_tools_suites.py`) and engine drivers (each runs one scenario through the isolated runner) |
| `e2e_video.py`, `e2e/` | the video scenes and their definitions |
| `feel_measure.py`, `feel/` | the feel matrix: input-to-screen latency, waits and stalls between peers, and its reducers |
| `heal_driver/` | the two-peer recovery harness several drivers build on |
| `fixtures/` | Lua scripts, input scripts and the recorded duel (`pickup_fire.ccreplay`) the drivers stage |
| `compare_*.py`, `snapshot_runtime.py`, `state_document.py` | readers and comparers of tick traces and checkpoint archives |
| `cross_peers.py`, `cross_report.py`, `world_*.py`, `soak_two_peer.py`, `acceptance_*.py` | the multi-machine acceptance harness |
| `relay_*.py`, `turn_relay_checks.py` | relay (TURN) tooling and the checks that no relay login is ever written to disk |
| `session_directory/` | the session directory service, its tests and a macOS launchd template |
| `contracts/` | the restoration contract audit and its Lua audit scripts |
| `pie_lockstep/`, `pie_writes/` | pie-menu regression arms |
| `sanitizers/` | sanitizer suppressions and their check |
| `linux/`, `macos/`, `mac/` | helpers for the POSIX builds and streams |
| `handtest/` | the two-window kit for playing a match by hand (`docs/handtest.md`) |
| `fonts/` | the font atlas tool |
| `box_facts.py`, `boxes.example.json` | the configured box file and its example |

## Utilities nothing else runs

Each is run by hand when its question comes up.

| Command | What it answers |
|---|---|
| `python tools/simdump_first_difference.py <left> <right>` | the first per-object row two simdumps disagree on, matched by uid |
| `python tools/check_state_inventory.py [--report]` | which data members of the snapshot classes neither a clone copies nor `Source/System/StateInventory.csv` classifies |
| `python tools/fonts/extend_font.py [<atlas> ...]` | rebuilds the Latin-1 cells of the font atlases in `Data` (and `fonts/latin1-contact-sheet.png`), keeping every ASCII and HUD cell |
| `python tools/runner_thread_probe.py --repo . --out <dir> -- <engine flags>` | the CPU mask and thread priorities the runner gives an engine |
| `python tools/session_directory/probe_pin_identity.py --repo . --out <dir>` | a client's pinned-certificate check against a directory whose certificate name differs: right pin, wrong pin, no pin |
| `python tools/relay_cloudflare_mint.py --turn-config <key file> --out <dir>` | whether the session directory can mint a Cloudflare relay login with that key |
| `python tools/relay_fixtures.py build\|check` | rebuilds or checks `relay_fixtures.json`, the reviewed list of login-shaped test values the relay sweeps excuse |
| `python tools/mac/check_cross_includes.py --base <commit> --out <file>` | whether each quoted include added since that commit names a tracked file with the same case (macOS and Linux file systems care) |
| `python tools/feel/draw_median.py <matrix root> ...` | the draw-time ratio over several feel matrices |
| `python tools/feel/world_report_audit.py <run root>` | a held world's progress from the host's receipts and both lobby counters |
| `python tools/h4_substitution_gates.py <gate> --out <dir>` | the nine live substitution gates of the reconnect design: a held seat given to an applicant over a real socket |
| `tools/pie_lockstep/`, `tools/pie_writes/` | pie-menu arms; each folder's README says how to run them |
