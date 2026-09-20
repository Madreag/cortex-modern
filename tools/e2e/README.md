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

Write `<capture>/stop-request.json` with a reason to stop an active capture through its runners. The driver retains
the stopped run and names unstarted checklist items in the aggregate review. After a hard interruption, once every
peer is stopped, `--finalize-only <capture>` recovers the saved indexes and produces missing media and metadata.
Add `--metadata-only` to leave media untouched; finalization uses this mode automatically at the scratch cap.
An exit code missing from an interrupted runner record stays unknown. Finalization never launches the engine.

The minimum set is `sp-smoke`, `mp-host-join`, `mp-reconnect-repair`, `world-late-join`, `ui-surfaces`,
`mod-void-wanderers`, `mp-leave`, `mp-rematch`, and `mp-rollback-lag`. The extended set adds
`mp-moderation`, `mp-host-migration`, `mp-resume-from-disk`, `mp-direct-vs-relay`, and
`mod-void-wanderers-multiplayer`. `menu-layout` records the settings geometry detector at three sizes. The single-process smoke includes the shipped
Scenario Battle picker and local play. Its FeelBaseline leg separately checks fixture startup and Lua errors.

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
The installed version 6 module names its activity and launcher scene `Void Wanderers` in `MissionActivities.ini`.

The scratch counter excludes symlinks and Windows reparse points. It stops at 5 GB and never removes a run tree.
`--scratch-root` selects the budget root; under `D:/mx` the default is the lane root containing the scenario.

Windows uses `run_sim_test`'s private-desktop runner and its existing fullscreen guard. The Scoop ffmpeg fallback is
Windows-specific; PATH and the Homebrew/Unix fallbacks are also supported. The POSIX runner selects
`<repo>/build-gns/CortexCommand`. Mac GL readback, runner isolation, codecs, and installed Python dependencies still
require an actual Mac capture.

## Ports

`tools/e2e_video.py` owns 49400-49479 and gives each run of a scenario one port from that block. No scenario may name a
port outside it: the readback detector owns 49180-49199, the launch driver 48320-48539 and 48630-48649.

## Retained state and prerequisites

`--run NAME` selects named runs; unselected items remain explicitly unrecorded in that invocation. Combine their
separate reviews when handing off a scenario recorded in several invocations. `--token NAME=VALUE` supplies tokens;
TURN_SERVER, TURN_USER and TURN_PASS also read the environment, with command-line values taking precedence.
An unresolved token skips its run with a harness finding. A run-level `blocked_by` skips the run with its stated
engine finding; item-level blockers retain context frames when the rest of the run can still be captured.

A restore run declares `start_when: {"run": "died", "peers": ["host", "client"], "ended": true}`. Each restoring
peer declares `retain_runtime_from: {"run": "died", "peer": "host"}` (its own peer name). The runner starts in that
same private runtime. No directory tree is copied or moved; the new launch, stdout and video have their own output
paths. The earlier console log is retained as a single file before restart. `resume_from` on the run selects the
checkpoint-owning peer. `{RESUME_MATCH}` and `{RESUME_TICK}` come from its newest complete checkpoint, restart
manifest and admission file; hashes of those files are retained in the scenario definition's checkpoint evidence.

`migration_gate: {"peers": ["clienta", "clientb"], "cap": 900}` requires both survivors to declare the same new host,
round and boundary. The `migration-hashes` gate checks every tick from boundary + 1 through the cap. It runs the
existing strict comparer and also compares every complete hash record, including total and controller, with no
exclusions. The original traces remain unchanged beside the gate report and its selected-range inputs.

`forbidden_log_regex` is the negative counterpart of `log_regex`; a matching line fails the item. Required module
versions are checked before launch through `requires_version`. The installed VW package declares 6.2.2 and is
blocked by the upstream 7.0.0 compatibility guard; its retained refusal is linked in both VW definitions.

The capture driver records assertions and frame ranges. Independent picture review remains a separate step.
