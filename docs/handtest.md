# Trying multiplayer by hand on one PC

One command gives you a HOST window and a CLIENT window of the game - windowed
1280x720, sound on, each with its own user directory so saves and settings never
collide - and you drive them through the game's own multiplayer lobby. A second
command packs both instances' logs for a bug report.

## Before you start

- A build of this tree: the folder that holds `Cortex Command.exe` and `Data\`
  (see Building in the README). Pass it as `-Build`. Below it is written `<build>`;
  `<kit>` means the folder containing `play.ps1` and `collect_logs.ps1`.
- Windows may ask to let the game through the firewall the first time an
  instance hosts. Allow it: the lobby runs over UDP.
- Run `play.ps1` from your own PowerShell 7 terminal. It refuses to open windows
  from an automation shell (when `CCCP_HEADLESS` is set in the environment) and
  says so. `-Headless` runs the same lobby flow on a hidden desktop, for
  automated checks.

## The one command

```powershell
pwsh <build>\tools\handtest\play.ps1 -Build <build>
```

Two windows appear side by side: HOST (left) and CLIENT (right). Each instance
runs from its own directory `<kit>\instances\<role>-<n>\` with its own
`Userdata\Settings.ini`, so profiles, saves and settings never touch each other.
Both scripts accept `-InstanceRoot <folder>` to use another instance folder.

> If a window opens on a "Rejoin Match?" offer instead of the main menu, that is
> last session's reconnect ticket doing its job - Cancel takes you to the menu.

> `NetworkShowDiagnostics = 1` is pinned on every instance: the match HUD shows
> a live net overlay (rtt, input delay, holds) - the on-screen counterpart of the
> log lines quoted below.

## Into a match (the game's own words)

HOST window:
1. Main Menu -> **Multiplayer**
2. Enter your callsign in **Name** -> **Host Game**
3. **Host Setup**: Activity = **P4 Alpha Duel - Base.rte** (proven in the
   self-check), Scene = **Grasslands**, Port = the port printed by play.ps1
   (default **47400**), **Players = the exact number of peers that will play**
   (2 for host + this client). The lobby refuses to start until that many peers
   are connected (Source/Network/NetLobbySession.cpp).
4. **Create Lobby** -> wait for the client to appear in the seats list.

CLIENT window:
1. Main Menu -> **Multiplayer** -> enter a callsign -> **Join Game**
2. **Host IP** `127.0.0.1`, **Port** `47400` -> **Connect**
3. In the lobby press **Ready**.

HOST: once every seat shows Ready -> **Start Match**. (Labels are from
`Data/Base.rte/GUIs/MainMenuSubMenuGUI.ini`.)

## Scenarios

Run them in order; the quoted lines are what the engine actually prints (they
land in each instance's `console-*.out.log` / `LogConsole.txt` and in
`match-report-*.json` at the instance root). "Wrong" = what failure looks like.

**A - Host, join, play ~2 minutes.**
Proves: transport, lobby, seat assignment, match start.
Proof in the client log:
`[net-match] auto input delay: peer <n> rtt <ms>ms -> <f> frames (manual floor <n>)` (Source/Network/NetMatchRunner.cpp)
`[net-match] recording the match to Userdata\Replays\...` (Source/System/ScenarioRunner.cpp)
Wrong: no `auto input delay` line, or `[net-session] admission refused reason=...` (Source/Network/NetSession.cpp).

**B - Lag the client: `-FakeLagMs 100`, then again with `200`.**
Adds a constant N ms round trip, N/2 per leg (Source/Network/GnsTransport.cpp).
Expect: your own input still feels instant (local prediction); the OTHER player
warps occasionally; nobody freezes.
Proof: the `auto input delay` line above shows the raised `rtt` and a larger
frame delay, and `match-report-*.json` shows per-peer `holds` and `pace`
(Source/Network/NetMatchService.cpp).
Wrong: `[net-match] hold peer=<n> frame=<f> AI in control` appearing in the
host log at 100ms (Source/Network/NetLockstep.cpp) means the lag
exceeded the slow-player bound.

**C - Client stalls ~5 s, then recovers.**
In a headed game alt-tab and Esc do not stall the client's network thread.
What does: freeze the process. Grab the client's PID from the launcher line
`[handtest] client pid=<PID>` (or Task Manager) and in another pwsh:

```powershell
Add-Type -MemberDefinition '[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(System.IntPtr h); [DllImport("ntdll.dll")] public static extern int NtResumeProcess(System.IntPtr h);' -Name Ntp -Namespace W
$p = Get-Process -Id <CLIENT_PID>; [W.Ntp]::NtSuspendProcess($p.Handle); Start-Sleep 5; [W.Ntp]::NtResumeProcess($p.Handle)
```

(Or drag the client window's title bar and hold it ~5 s - the message pump stops.
To see what stalled looks like without timing pressure, use resmon.exe ->
right-click the process -> Suspend Process / Resume Process.)
Expect: host keeps playing; the client's seat shows `held - AI in control`
(HUD toast - Source/System/ScenarioRunner.cpp); the client resumes by itself.
Proof in the host log:
`[net-match] hold peer=<n> frame=<f> AI in control` (Source/Network/NetLockstep.cpp)
then on recovery `[net-match] private rejoin peer=<n> incarnation=<m>`
(Source/Network/NetMatchService.cpp) or `[net-match] rejoin: <name>
reconnected - resyncing the match` (Source/Network/NetMatchService.cpp)
and `[net-match] seat-reclaimed peer=<id> ...` = the human taking the seat back
(Source/Managers/MovableMan.cpp).
Wrong: the host freezes waiting on the client's input (no `hold peer=` line).
A long enough stall drops the peer instead - the leave lines under D apply.

**D - Client quits mid-match.**
Close the client window. Host keeps playing; the seat stays held by AI (the
`held - AI in control` toast is on screen only - Source/System/ScenarioRunner.cpp).
Proof in the host log:
`[net-match] <peer> left the match at frame <f> (<reason>)` (Source/Network/NetLockstep.cpp)
(or, if the peer was already held: `[net-lockstep] a leave becomes a hold for peer <n> at frame <f>: <detail>` - Source/Network/NetLockstep.cpp).
Relaunch the client (`-Role client`) and pick **Rejoin Match** on the landing
screen: the `rejoin:` and `seat-reclaimed` lines from C prove the seat hand-back.
Wrong: host match ends when the peer leaves, or the rejoined player lands in a
fresh seat instead of the held one (no `seat-reclaimed` line).

**E - Autosave on.**
HOST: Options -> **Recovery** -> **Autosave checkpoints** on (interval
"sim seconds, 30-3600" - Data/Base.rte/GUIs/MainMenuSubMenuGUI.ini; parsed at
Source/Managers/SettingsMan.cpp). Checkpoints land in `Autosaves\` of the instance
(Source/System/AutosaveStore.cpp).
Expect: the match keeps flowing at each checkpoint - a small hitch is OK, a stall is not.
Proof (each peer's log):
`[autosave] tick=<n> capture_ms=<n> bytes=<n>` (Source/Managers/ActivityMan.cpp)
`[autosave] agreed match=<hash>` (Source/Network/NetMatchService.cpp)
Load one via a rejoin/resume: `[autosave] restored match=<hash> ...` (Source/Managers/ActivityMan.cpp).
Wrong: `[autosave] failed tick=<n> reason=...` (Source/Managers/ActivityMan.cpp) - the checkpoint did not land.

**F - Rematch.**
After the match ends, both peers are back in the lobby seats
(Source/Menus/MainMenuGUI.cpp). Client presses **Ready**, host **Start Match** again.
Proof: `NETWORK: Match complete: <result>` in each peer's LogConsole.txt
(Source/Main.cpp) - the same line in both collected instances proves both
sides saw the same match end; the new match writes a fresh `match-report-*.json`
`last_match` block (Source/Network/NetMatchService.cpp).
Wrong: `[net-match] rematch unavailable: ...` (Source/Network/NetMatchService.cpp).

**G - Kick and ban.**
HOST, in the lobby's **Moderation** / seats panel (visible in the lobby right
column): **kick** the client's seat, then **ban** the client's seat. The host
sees `Seat <n>: <action> accepted.` and the log gets
`[net-moderation] <action> seat=<n> applicant=<id> result=<result>`
(Source/Network/NetReconnectUx.cpp).
The banned client re-dials and is refused:
`[net-reconnect] admission refused reason=ParticipantBanned peer=<id> ...` (Source/Network/NetReconnectSession.cpp)
and sees `The host banned you from this session` (Source/Network/NetSession.cpp).
The ban list persists in `host-bans.txt` at the host instance root and reloads
every time the host opens a lobby (Source/Network/NetMatchService.cpp) - finish the
scenario with **Remove ban** (or delete the file) or the client stays banned
next launch.
Wrong: the refused line names a reason other than ParticipantBanned, or the
kicked seat still holds the player.

**H - Mac as a third (or second) peer.**
Relaunch with `-Mac` - it prints this PC's LAN IP and the command for the Mac:

```
git -C <this repository on the Mac> rev-parse HEAD    # must print the build sha the launcher showed
cd <this repository on the Mac> && ./build-gns/CortexCommand
```

The Mac checkout must be the same commit the PC build was built from (the sha
is printed in the launcher output). On the Mac: MULTIPLAYER -> Join Game ->
Host IP = the printed LAN IP, Port = the session port -> Connect -> Ready.
Set the host's **Players** to the exact total peer count (Source/Network/NetLobbySession.cpp):
Mac alone with the host = **2**; PC client + Mac = **3**; two PC clients + Mac = **4**.
Proof: an `auto input delay` line per remote peer (peers 2 and 3 -
NetMatchRunner.cpp) and `match-report-*.json` `players`/`human_seats`
counting all of them (Source/Network/NetMatchService.cpp).
Wrong: host's Players count higher than the peers present - Start stays disabled
(no `wait_all_ready` reachable).

## Collecting evidence

```powershell
pwsh <build>\tools\handtest\collect_logs.ps1 -Out <a folder for the logs>
```

Copies every instance's console logs, runner records, match reports, autosaves,
replays, crash dumps and the effective Settings.ini into
`<Out>\<role>-<n>\` plus a `MANIFEST.txt` (build sha + dirty flag of the exe's
tree, exe hash, exact command lines, per-file sizes/mtimes). `reconnect.ticket`
is not collected - it is a live rejoin credential. Self-check runs under
`<kit>\instances\_selfcheck\` are never mixed in; collect them explicitly with
`-InstanceRoot <kit>\instances\_selfcheck` if needed. When launching with a custom
`-InstanceRoot`, pass the same folder to the collector.

WARNING: never delete `<kit>\instances` with a tool that follows junctions -
each instance's `Data` is a junction into the build tree and a junction-following
delete would eat the build. Remove each instance's `Data` junction itself before
deleting the instance folder; never enter the junction's target.

## Ports

Default block 47400-47419 (the kit's own; the automated tests in this tree use
bases from 47563 up, or 41210). Override with `-Port <base>`.
