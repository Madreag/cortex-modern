# End-to-end video scenarios

Each `<name>.json` here is one play-through `tools/e2e_video.py` can run on private hidden desktops. The engine's
`-record-video <dir>` flag writes the frames; the driver encodes them, tiles a contact sheet and writes `review.json`.
A reviewing agent reads `review.json` first, then the contact sheet, then the video, and only an agent-approved
capture is parked for the user.

    python tools/e2e_video.py --repo <tree> --out D:\mx\e2e-video\<name> --scenario <name>
    python tools/e2e_video.py --repo <tree> --review-only D:\mx\e2e-video\<name>

`--review-only` prints the retained review without launching, encoding, or rewriting it. `--size` overrides every
run's logical resolution. `--fps` sets the capture and encoded frame rate. The encoder uses each saved frame's
wall-clock timestamp; a missing render interval remains visible as a held frame in the MP4.

The minimum set is `sp-smoke`, `mp-host-join`, `mp-reconnect-repair`, `world-late-join`, `ui-surfaces`,
`mod-void-wanderers`, `mp-leave`, `mp-rematch`, and `mp-rollback-lag`. The single-process smoke includes the shipped
Scenario Battle picker and local play. Its original, unchanged FeelBaseline leg remains a separate prerequisite.

## The file

```jsonc
{
  "schema": 1,
  "name": "mp-host-join",
  "title": "one line the reviewer reads first",
  "requires": ["VoidWanderers.rte"],   // data modules that must exist under <repo>/Data before the run
  "size": "960x540",                   // the logical resolution, overridable per run and by --size
  "timeout_s": 420,
  "runs": [                            // several runs when a scenario needs more than one launch
    {"name": "join", "size": "1280x720", "peers": [ /* … */ ]}
  ],
  "peers": [                           // used when the scenario has a single run
    {"name": "host",
     "args": ["-net-match-service-e2e", "-net-port", "{PORT}"],
     "menu_script": "mp-host-join.host.menu.txt",   // inline under "scripts", or a file in this folder
     "input_script": "play-input.txt",
     "probe": "mp-host-join.host.probe.json",
     "settings": {"NetworkDisplayName": "Host"},    // seeded into the run's own Settings.ini
     "env": {},
     "start_delay_s": 2.0,                          // seconds to wait BEFORE this peer is launched
     "kill_after_s": 45,                            // drop this peer through its runner after N seconds
     "runtime_files": [{"copy": "tools/feel/FeelBaseline.lua", "to": "Userdata/UserScenes.rte/FeelBaseline.lua"}]}
  ],
  "scripts": {"name.txt": "inline text, when a script is short"},
  "checklist": [
    {"id": "lobby-seated", "peer": "client", "what": "the lobby lists both players and the picked activity",
     "screen": "MultiplayerScreen", "sim_ticks": [0, 0],
     "assert": "menu script: assert_label LabelLobbyPlayer1 Joiner"}
  ]
}
```

Tokens the driver substitutes in `args`, `env`, `settings` and every script: `{REPO}`, `{PORT}`, `{PEER}`, `{OUT}`,
`{STAGE}`, `{PROBE_DIR}`, `{MENU_SCRIPT}`, `{INPUT_SCRIPT}`, `{VIDEO}`, `{SIZE}`, `{WIDTH}`, `{HEIGHT}`, `{FPS}`, and
per peer `{STAGE_<peer>}`, `{PROBE_DIR_<peer>}`, `{VIDEO_<peer>}` so one peer's script can wait on another's done file.

## The checklist is the reviewer's contract

Every step names the screen or control that must be visible and the sim-tick range it must be visible in. The driver
resolves each item against the capture's own `frames.jsonl` and writes the frame range into `review.json`, beside the
verdict the peer's menu probe recorded. An item with `"frames": null` was never seen on the video and is a finding, not
a formatting problem. An item carrying `"blocked_by"` names a seam that does not exist yet; it is reported, never hidden.

Use `"run"` to assign a checklist item to one named run. A `video_mark <id>` command works in both menu scripts and
menu probe steps. Set the item's `"mark"` to that id to bound its evidence by the next mark. Menu items list required
regular expressions in `"events"`; probe items retain the executed assertions between their marks. `"probe_steps"`
can name explicit probe indices, and `"log_regex"` checks retain matched log lines with line numbers. The feel scenario
uses the unchanged `tools/feel/report.py` gates through `"gate"`.

`frames` contains zero-based MP4 frame numbers. `capture_frames` names the original PNG/index range, and
`video_seconds` gives the corresponding wall-clock interval. Every item's `what` states the visible result to review.
The top-level `review.json` joins the per-run reviews. `manifest.json` retains the source tip and dirty state, executable
hash, resolution/rate, elapsed time, frame count, MP4 size/hash, contact sheets, and each recorder manifest. A failed
probe or missing range remains a finding even when the engine exits zero. A planned process drop retains its actual
nonzero runner exit code and termination reason.

Peers can start after `start_when: {"peer": "host", "event": "video_mark host-listening"}`, after a recorded gameplay
tick (`"sim_tick": 900`), or after another peer ends (`"ended": true`). `kill_at_tick` drops a peer through its runner
after that peer records the requested tick; its `injected-drop.json` records the exact last captured frame. A peer with
`observe_after_failure` keeps the observer peers running while its own failure remains in the verdict.

The driver writes `<stage>/gameplay-started.json` only after the recorder reports a gameplay frame. Single-player
probes can wait for that file before interpreting the menu's simulation counter as gameplay progress. Menu visibility
holds use `wait_ms`; probes use `elapsed_ms`, since an unrestricted menu can render hundreds of frames per second.

The UI scenario serves the project's real session directory locally over TLS on port 49479. Each run retains its
certificate, listing observations, and service log. The listed-game selector is `activate GameRow0` (zero-based).

Missing required modules produce a `requires-missing` review and manifest without starting the engine. Void Wanderers
must be installed as `<repo>/Data/VoidWanderers.rte`; its download page is
https://mod.io/g/cccp/m/void-wanderers-by-weegee. Verify the installed activity names before capturing that scenario.

The scratch counter excludes symlinks and Windows reparse points. It stops at 5 GB and never removes a run tree.
`--scratch-root` selects the budget root; under `D:/mx` the default is the lane root containing the scenario.

Windows uses `run_sim_test`'s private-desktop runner and its existing fullscreen guard. The Scoop ffmpeg fallback is
Windows-specific; PATH and the Homebrew/Unix fallbacks are also supported. The POSIX runner selects
`<repo>/build-gns/CortexCommand`. Mac GL readback, runner isolation, codecs, and installed Python dependencies still
require an actual Mac capture.

## Ports

`tools/e2e_video.py` owns 49400-49479 and gives each run of a scenario one port from that block. No scenario may name a
port outside it: the readback detector owns 49180-49199, the launch driver 48320-48539 and 48630-48649.
