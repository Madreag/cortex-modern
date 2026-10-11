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
`ssh`, `os` (`win32` or `darwin`), `hostname`, `repo`, `scratch`, `python` and
`build_receipt`. The Windows entry also supplies `session_dir` and `task`; the
Mac entry supplies `binary`, relative to its checkout.

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

The Mac's headed window uses normal game presentation; the headless render
settings option applies to the Windows host. The Mac records `render_cap: null`
and `presentation: headed`. Both roles retain the scene's recording and pace
assertions. No operating-system input is used.
