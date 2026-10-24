# Scenes with players on separate boxes

Run the scene from a committed, clean checkout. The controller runs Python only.
Each assigned box builds the same commit in its own checkout. A Windows host runs
headless through its signed-in session task and foreground probe. A Mac joiner
uses the existing headed POSIX runner and its localhost GUI-session hop.

```
python tools/e2e_peers.py --scenario fight15-s02 --out <new-capture-folder> \
  --peer-boxes host=<Windows-box>,joiner=<Mac-box> --boxes-file <private-box-facts.json>
python tools/e2e_peers.py --check-spread <capture-folder>
python -m unittest discover -s tools -p test_e2e_peers.py
```

`--scenario` accepts a scene name or a JSON file path. The original role scripts,
settings, dimensions, timeouts, picture assertions and pace checks are retained.
The players discover the public game through the scene's directory and select
its relay. Declared probe completion files are copied with the existing spread
runner's complete-file checks. Game traffic uses the relay.

Keep box facts outside the source tree. Each entry in `boxes` supplies `name`,
`ssh`, `os` (`win32`, `darwin` or `linux`), `hostname`, `repo`, `scratch`, `python` and
`build_receipt`. The Windows entry also supplies `session_dir` and `task`; a
Mac or Linux entry supplies `binary`, relative to its checkout.

Each box's build receipt supplies `commit`, `build_exit_code`,
`executable_sha256`, `build_log` and `build_log_sha256`. The runner checks the
checkout, executable, successful build log and unchanged scene before launching.
Keep that receipt and log available until the role finishes.

The capture retains both players' native `launch.json`, scripts, probes, logs,
movies and contact sheets, with one combined `capture.json` and `review.json`.
`topology: spread` and box names describe placement. The separate-box detector
checks retained process records and their actual host names. Its GREEN result
proves placement; `review.json` separately records the scene's original findings
and `scripted_result`. Refused launches retain a RED review and their session log.

A scene may drop, hang and restart its players. Each player's box keeps one folder
for the whole scene, so a later run restarts the player beside its earlier runtime:
`kill_when`, `kill_at_tick` and `kill_after_s` end the engine through its own
runner, `suspend_when` with `suspend_s` stops the whole engine for that long and
lets it run on (`injected-suspend.json` in its probe folder), and
`retain_runtime_from` names the player's own earlier run, or an earlier player of
its own run that it starts after (`start_when` on that player's end). A player
restarted in its run plays that player's seat on that player's box: `--peer-boxes`
names one box for both. A `resume_from` run reads the save on the box that wrote it
(`--checkpoint`); only its match and tick come back. A gate on another player's end,
log line, event, tick or probe is met by the facts that player's box reports; a
restart from another player's runtime of an earlier run and a host start that waits
on a joiner are refused. Runtimes and saves stay on their boxes; the killed run's
partial recording is collected. A host with no menu names its listed lobby with
`lobby_file`.

`--one-box` plays every player on the collector's own box, each through its own
player run and folder, the way the runners split them. Its capture is a detector:
`topology: single-box`, `proof: false`.

```
python tools/e2e_peers.py --scenario mp-resume-from-disk-relay --out <new-capture-folder> \
  --peer-boxes host=<BOX>,client=<BOX> --boxes-file <private-box-facts.json>
python tools/e2e_peers.py --scenario mp-reconnect-relay --out <new-capture-folder> \
  --peer-boxes host=<BOX>,dropped=<BOX>,rejoined=<the dropped player's BOX> --boxes-file <private-box-facts.json>
```

The Mac's headed window uses normal game presentation; the headless render
settings option applies to the Windows host. The Mac records `render_cap: null`
and `presentation: headed`. Both roles retain the scene's recording and pace
assertions. No operating-system input is used.
