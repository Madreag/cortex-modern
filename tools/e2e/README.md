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

The scratch counter excludes symlinks and Windows reparse points. It stops at 5 GB by default and never removes a run tree.
`--scratch-root` selects the budget root; under `D:/mx` the default is the lane root containing the scenario.
An explicitly granted allowance can be selected with `--scratch-limit-bytes 8000000000`. The driver checks that
allowance before launch, while peers run and before media writes; `capture.json` and `manifest.json` retain it.
Finalization keeps the recorded root and allowance unless an explicit command-line replacement is supplied.

Windows uses `run_sim_test`'s private-desktop runner and its existing fullscreen guard. The Scoop ffmpeg fallback is
Windows-specific; PATH and the Homebrew/Unix fallbacks are also supported. The POSIX runner selects
`<repo>/build-gns/CortexCommand`, or `CCCP_TEST_BINARY` when set; manifest hashing uses the same selection. Mac GL readback, runner isolation, codecs, and installed Python dependencies still
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

`frame_gap: {"max_ms": 1000, "screens": [...], "ignore_screens": [...]}` measures the recorder's wall clock between
consecutive presented frames and fails the item on a longer hold: a held frame in a wall-clock capture is a render
stall, not a dropped frame. Frames stamped with an ignored screen (the game and the loading screens by default) are
not measured. The evidence names the worst gap and every gap over the bound with its frames and screen.

`forbidden_log_regex` is the negative counterpart of `log_regex`; a matching line fails the item. A scenario can
require an exact module version through `requires_version`. The Void Wanderers scenarios require the installed
module but impose no version prerequisite: its 6.2.2 declaration alone does not prevent a capture on 7.0.0.
Record the actual load and play result. A version warning alone is evidence, while a process abort, failed activity
start or Lua error is a finding with the exact log line; neither the module nor the engine guard is altered.

The capture driver records assertions and frame ranges. Independent picture review remains a separate step.

The lockstep wait loop presents through its own renderer. The capture hook on its existing event poll reads the
preceding completed GL presentation, tagged `LockstepWaitOverlay`, on the next poll. The frame timestamp is the
readback time, so a wait-overlay picture can lag its native draw event by one poll. Recorded simulation ticks and
native handover draw timestamps remain separate evidence; the video does not interpolate motion across a stall.

A peer with `gameplay_epochs: 2` also receives `<stage>/gameplay-epoch-2.json` when saved gameplay frames show the
simulation counter reset. Rematch probes wait for that record before measuring their second play window.

`record_tick_hashes` is a menu-script command for a lobby with `-out` and a positive `-max-ticks` already configured.
With video recording active, it arms the existing native hash path and collector for the next round. The recorder's completion action exports that collector before engine shutdown. This lets a rematch capture end round one normally and
trace round two through its cap. `hash_gate` names the two trace peers, first tick and cap; it uses the same complete
record comparison as the migration gate. The command does not alter a wire flag, hash mask or comparison rule.

`mp-rematch --run rematch` records the ordinary lobby, End Match and second round, with every complete hash record
compared from tick 1 through 600. Its separate `--run injected-desync` uses the existing
`-determinism-selftest-perturb -determinism-selftest-perturb-tick 240` seam on the host and
`-net-match-e2e-resync` on both peers. It never presses Repair Match. Its checklist requires the native divergence,
snapshot reload and relaunch lines, recorded repair-overlay frames, the visible healed-frame toast and subsequent
Running gameplay through tick 900. A run of one arm does not prove the other.

An item's `readback` checks recorded probe observations by step and field path. A toast observation can require
both exact visibility and text without blocking all later capture steps when the expected toast is absent.

## One peer per machine

`mp-host-join-cross` launches only the selected local peer. Start the Windows host first, then the Mac client with
the Windows machine's reachable address. The scripts dial the address and port through the separate join fields;
neither peer waits for a file on the other machine. Both halves record their live session, round and configuration hash.

```text
python tools/e2e_video.py --repo <windows-tree> --scenario mp-host-join-cross --peer host --out <host-capture> --fps 6
python tools/e2e_video.py --repo <mac-tree> --scenario mp-host-join-cross --peer client --token HOST_ADDRESS=<windows-address> --out <client-capture> --fps 6
```

`HOST_ADDRESS` may also come from the environment. This definition uses a plain address on port 49412; it does not
depend on a directory listing. The address must be reachable from the client and the two binaries must be compatible.
After transferring the complete Mac capture through the lead's approved evidence workflow, join its contract with
the host's retained capture:

```text
python tools/e2e_video.py --merge-peer-captures <host-capture> <client-capture> --out <merged-contract>
```

The merge reads both halves and writes metadata only. It requires the same checklist, live session, round and
configuration hash, complementary host/client roles, distinct peer ids, and unchanged MP4/contact-sheet hashes.
It preserves each machine's executable hash, source tip, command and timing. A Windows-only pair is labelled
`local-pair-validation-only`; it is not a Mac capture. Picture review remains pending after a successful merge.

On the Mac, both this definition and `sp-smoke` need Python, `date`, ffmpeg/ffprobe on PATH (or a listed Unix path),
the matching data modules, and a built executable selected by `CCCP_TEST_BINARY` or `build-gns/CortexCommand`.
Pillow supplies labelled contact sheets; the ffmpeg fallback is unlabelled. The binary must include `wait_label`,
`dump_match_identity` and the recorder flags. `CCCP_TEST_DATA` and `CCCP_TEST_SETTINGS` can select the data and settings.
The POSIX runner supplies the runtime, `CCCP_HEADLESS=1`, `SDL_MAC_BACKGROUND_APP=1` and its dylib search path;
a working native display/GL context is still required. The engine requests an SDL hidden window in both creation paths.
Windows private-desktop isolation has no POSIX equivalent in this driver. The Mac lane must verify its own display
isolation, GL frame readback, encoding and runtime dependencies. No Mac execution is implied by these definitions.
Reading the scripts and runtime setup reveals no additional Mac-specific change needed by `sp-smoke`.

A UI-only readback may end a peer with `kill_when: {"peer": "survivor", "probe_complete": true}`. The driver waits
for a complete, successful native probe, then terminates through the runner and retains the drop receipt. A failed or
partially written probe never satisfies that gate. `menu-host-loss` uses this to retain the larger-size status readbacks
without extending them into a second migration acceptance run.

The pause settings skin has Video, Audio, Input, Gameplay and Misc only. Its Network tab is intentionally absent
(`SettingsGUI.cpp` selects `SettingsPauseGUI.ini`); the main-menu settings walk covers the six Network subpages.
