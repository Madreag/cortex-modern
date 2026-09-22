# tools/handtest - the hand-test kit

`play.ps1` launches isolated multiplayer instances of the game; `collect_logs.ps1`
packs their evidence for review. The full runbook is `HANDTEST.md` at the root.

## Isolation

Each instance runs with its working directory set to its own runtime at
`D:\mx\handtest\<role>-<n>\` (`host-1`, `client-1`, `client-2`). The runtime holds
`Mods\`, `ScreenShots\`, `Userdata\`, `Temp\`, `Autosaves\`, a `Data` junction to
the build's data, and a `Userdata\Settings.ini` copied from the tree's template
and patched to: 1280x720 windowed, VSync off, sound on, `DeltaTime = 0.016667`
(the deterministic timestep), `SkipIntro = 1`, the lobby name, and the lockstep
pins the harness uses (`LocalPrediction`, `LocalPredictionMaxTicks`,
`NetworkHostDelayPolicy = Auto`, `NetworkInputDelayFrames`,
`NetworkSlowPlayerBoundTicks`, `NetworkSlowPlayerPolicy = Substitute`,
`NetworkShowDiagnostics`). The engine has no user-directory flag - the working
directory IS the isolation. Saves, settings and autosaves never collide.

## Run

```powershell
pwsh tools\handtest\play.ps1                          # host + client, headed
pwsh tools\handtest\play.ps1 -Role client -FakeLagMs 100
pwsh tools\handtest\play.ps1 -Mac                     # host + Mac join instructions
```

Headed instances are windowed 1280x720 and placed side by side (host left).
Lobby port: `-Port` (default 47400; the kit owns 47400-47419, below every harness
block). Flags passed are limited to those in `Source/Main.cpp` (`-headed`,
`-net-match-report`, `-net-reconnect-ticket`, `-net-host-bans`, `-net-fake-lag`);
`-LossPct` sets `CC_TEST_GNS_LOSS_PERCENT`, which the engine only honors
headless+lockstep. Each instance writes `kit-launch.json` (argv, env, exe sha256)
into its runtime dir.

## Logs

```powershell
pwsh tools\handtest\collect_logs.ps1 -Out <dir>
```

Produces `<dir>\<instance>\` (console log, `run\stdout.log`, `launch.json`,
`match-report.json`, `Autosaves\`, crash dumps, effective Settings.ini) plus
`<dir>\MANIFEST.txt` with build sha, exe hash, timestamps and command lines.

## Headless self-check (workers, not users)

```powershell
pwsh tools\handtest\play.ps1 -Headless -Role both -Clients 1 -Build <Final build dir>
```

Every launch goes through `tools/isolated_launch.py` (private hidden desktop,
`CCCP_HEADLESS=1`, job object). Each instance runs `-headless -menu-script
selftest.menu.txt -net-match-ticks 1200 ...`: the script drives the real lobby UI
(Multiplayer -> Host Game / Join Game -> Create Lobby / Connect -> Ready ->
Start), a bounded match runs ~20 sim-seconds, both peers return to the lobby and
`exit` cleanly. Exit code 0 = pass; logs land in `<runtime>\run\`.
