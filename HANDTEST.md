# Multiplayer hand-test runbook

One command gives you a HOST window and a CLIENT window of Cortex Command on this
PC - windowed, 1280x720, sound on, each with its own user directory under
`D:\mx\handtest\<role>-<n>\` so saves and settings never collide. A Mac on the LAN
can join as an extra peer. One more command collects every log for review.

## Prerequisites (once)

1. A build: the kit defaults to this tree's own `Cortex Command.exe`. For a
   different Final build pass `-Build <dir containing the exe>`.
2. Firewall: the host instance binds a TCP listen port. Run **elevated** once:

   ```
   pwsh -NoProfile -File D:\Projects\reviews\takeover-20260909\grok-workers\firewall_allow_all_exes.ps1
   ```

   It adds an inbound-allow rule for every `Cortex Command*.exe` under
   `D:\Projects`. If your `-Build` lives outside `D:\Projects`, allow that exe in
   Windows Defender Firewall yourself or the first host launch prompts.

## The one command

```powershell
pwsh -NoProfile -File tools\handtest\play.ps1
```

Defaults: `-Role both -Clients 1 -Port 47400` - HOST window left, CLIENT window
right. Useful switches:

| Switch | Effect |
|---|---|
| `-Role host` / `-Role client` | start only that side |
| `-Clients 2` | two local client windows |
| `-Port <n>` | different lobby port (kit block 47400-47419; default 47400) |
| `-FakeLagMs 100` | fake send/recv lag on the client (`-net-fake-lag`, `Source/Main.cpp:1508`) |
| `-LossPct <n>` | sets env `CC_TEST_GNS_LOSS_PERCENT` (`Source/Main.cpp:5056-5071`) - headless+lockstep only, **inert in a headed window** |
| `-Mac` | no local client; prints the exact Mac join command instead |
| `-Headless` | self-check only (runners, private desktop, CCCP_HEADLESS=1) |

## Playing through the lobby (the game's own words)

Both windows land on the main menu.

1. In BOTH windows: **Multiplayer**. Type a name at **Multiplayer name:**.
2. HOST window: **Host Game** -> set **Port** to `47400`, **Players** to `2`
   (3 if the Mac joins) -> **Create Lobby**.
3. CLIENT window: **Join Game** -> **Host IP** `127.0.0.1`, **Port** `47400` ->
   **Connect**. You land in the **Lobby**; press **Ready**.
4. HOST window: **Start Match** lights up once every peer is ready. Press it.
5. In the lobby the host's **Host Options** button opens the pages used below:
   **Seats** (kick/ban), Rules, Network, Recovery (**Autosave checkpoints**),
   Files, Session. In match, Esc opens the pause menu; **End Match** is host-only.
   Post-match the lobby offers **Start Match** again (rematch).

## Scenarios, in order

What to look for, what "wrong" looks like, and the exact log line that proves the
mechanism. After the session, `collect_logs.ps1` gathers each instance's
`console.out.log` / `run\stdout.log` where these lines live.

### A. Host/join and play ~2 minutes

- Look for: client reaches the Lobby, **Ready** -> host **Start Match** -> both
  peers play; inputs work on both sides.
- Wrong: client stuck in **Join Game** / "connection refused", lobby shows 1
  member, or either window freezes at start.
- Proof in logs: `[menu-script]`-free manual run still emits the lockstep lines -
  on the host, `[net-match] ...` activity (e.g.
  `Source/Network/NetLockstep.cpp:978` prints `[net-match] seat-reclaimed peer=...`
  once seats bind) and on both, the lobby roster. In the self-check the proof is
  `connected:2 -> OK` and `assert_substate expected=Lobby actual=Lobby PASS`.

### B. Fake lag on the client (`-FakeLagMs 100`, then 200)

- Quit the client (window X), relaunch it alone:
  `pwsh tools\handtest\play.ps1 -Role client -FakeLagMs 100`, rejoin, play. Then
  repeat with 200.
- Look for: your own input stays instant; the remote player warps occasionally;
  nobody freezes.
- Wrong: both sides stutter (lag applied to host too), or the client's own input
  delays (that would mean the flag hit the wrong peer).
- Proof: `Source/Main.cpp:1508` parses `-net-fake-lag`;
  `Source/Network/GnsTransport.cpp:143-148` applies the fake send/recv lag. The
  client window's `console.out.log` carries the flag in `kit-launch.json`'s argv.

### C. Client pauses / alt-tabs ~5 s

- Look for: host keeps playing; the client's seat shows the toast
  **held - AI in control**; the client rejoins by itself.
- Wrong: host stalls on a "waiting for peer" screen, or the client never resumes.
- Proof: `Source/System/ScenarioRunner.cpp:1417-1425` emits
  `PushNetUiToast("seat_held", "held - AI in control", peerId)`; the rejoin log on
  the host is `[net-match] rejoin: <name> reconnected - resyncing the match`
  (`Source/Network/NetMatchService.cpp:~4672`).

### D. Client quits mid-match

- Close the client window entirely.
- Look for: host keeps playing; the departed seat is taken by AI (same
  **held - AI in control** toast) - the round survives.
- Wrong: host match aborts, lobby dissolves, or the seat freezes.
- Proof: `[net-match] seat-reclaimed peer=... frame=... live_actors=...`
  (`Source/Network/NetLockstep.cpp:978`) and
  `[net-match] <peer> left the match at frame ...`
  (`Source/Main.cpp:~8063`). The client can rejoin later - its runtime dir keeps a
  `-net-reconnect-ticket` (`Source/Main.cpp:1101`); use **Rejoin Match** /
  **Resume Match** on the multiplayer landing.

### E. Autosave

- Before starting: in the host's **Host Options -> Recovery** page turn on
  **Autosave checkpoints** (interval in sim seconds, 30-3600; the setting key is
  `AutosaveSeconds`, parsed at `Source/Managers/SettingsMan.cpp:~358`).
- Look for: at each interval the match keeps flowing (a brief hitch is normal);
  afterwards, a fresh lobby offers to resume the saved match and the reload works.
- Wrong: capture errors, peers desyncing off a checkpoint, resume button absent.
- Proof: `[autosave] tick=... capture_ms=... bytes=...`
  (`Source/Managers/ActivityMan.cpp:572`), `[autosave] agreed match=... tick=...`
  (`Source/Network/NetMatchService.cpp:2718`), and on reload
  `[autosave] restored match=... tick=...` (`ActivityMan.cpp:~1581`). Files land in
  `<runtime>\Autosaves\` (`Source/System/AutosaveStore.cpp:67`) - collect_logs
  copies them.

### F. Post-match rematch

- Let a match end (host pause menu -> **End Match**, or a win). Both peers return
  to the same live lobby.
- Look for: **Start Match** re-arms once everyone readies again; round 2 plays.
- Wrong: lobby dead/expired, Start Match greyed forever.
- Proof: `Source/Menus/MainMenuGUI.cpp:3193-3197` returns completed matches to the
  lobby (`g_NetMatchService.ReturnToLobby()`); service lines
  `[net-match-service-e2e] rematch: ...` bracket the flow, and a refusal prints
  `[net-match] rematch unavailable: ...`
  (`Source/Network/NetMatchService.cpp:~739`).

### G. Kick and ban

- In the lobby (or during a match pause), host: **Host Options -> Seats**; select
  the client's row -> **Kick player**. Repeat and choose **Ban this session**.
  The **Banned players** list offers **Remove ban**.
- Look for: kicked peer leaves with a notice; a banned peer's rejoin is refused;
  the host sees `<host> removed seat <n>` / `<host> banned seat <n>`.
- Wrong: the seat stays occupied, or a banned peer rejoins silently.
- Proof: `[net-moderation] <action> seat=<seat> applicant=<id> result=<result>`
  (`Source/Network/NetModerationUx.cpp:321-329`);
  `[net-reconnect] admission refused reason=ParticipantBanned ...`
  (`Source/Network/NetReconnectSession.cpp`); bans persist in the host instance's
  `host-bans.txt` (`-net-host-bans`, `Source/Main.cpp:1106`).

### H. Mac as a third peer

- Run `pwsh tools\handtest\play.ps1 -Mac` (host + printed Mac command, no local
  client), or keep a local client and just read the block it prints.
- On the Mac: `cd <checkout of this tree> && ./build-gns/CortexCommand`, then
  **Multiplayer -> Join Game -> Host IP** = this PC's LAN address (printed by
  `-Mac`), **Port** `47400` -> **Connect -> Ready**.
- The host must set **Players** to 3 before **Create Lobby**.
- Look for: real LAN ping; same lobby behavior as local.
- Wrong: the Mac can't reach the host (check the firewall rule above and that the
  host binds the printed port).
- Proof: same `[net-match]`/`dump_lobby` roster lines show 3 members.

## Collecting logs for the lead

```powershell
pwsh -NoProfile -File tools\handtest\collect_logs.ps1 -Out D:\mx\handtest\collected
```

One folder: `host-1\`, `client-1\`, ... each with console/engine logs, autosaves,
crash dumps, settings, and `MANIFEST.txt` (build sha, exe hash, timestamps,
command lines, runtime paths).
