# End-to-end video scenarios

Each `<name>.json` here is one play-through `tools/e2e_video.py` can run on private hidden desktops. The engine's
`-record-video <dir>` flag writes the frames; the driver encodes them, tiles a contact sheet and writes `review.json`.
A reviewing agent reads `review.json` first, then the contact sheet, then the video, and only an agent-approved
capture is parked for the user.

    python tools/e2e_video.py --repo <tree> --out D:\mx\e2e-video\<name> --scenario <name>
    python tools/e2e_video.py --repo <tree> --review-only D:\mx\e2e-video\<name>

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

## Ports

`tools/e2e_video.py` owns 49400-49479 and gives each run of a scenario one port from that block. No scenario may name a
port outside it: the readback detector owns 49180-49199, the launch driver 48320-48539 and 48630-48649.
