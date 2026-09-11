# W5 evidence package: audio-checkpoint restore failure on `peers_3_4_regression` repeat 3

Job: `D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527`
Gate: `peers_3_4_regression` (plan id `h4_peers_3_4_regression_3`, `D:\mx\s41b3\plan.json` lines 844–858, `meta.repeat=3`)
Pin: `D:\mx\s41b3\pin.json` `exe_sha256=bb3cf264b4e7304f45486440998e4b63e37d5d46d0d119caa04f76c2fc378ffb` `source_head=c8f8188ae0b4f4524f83ee5cdaebf4ef0ec3e49e`
Approved-tree HEAD at collection: `9751a90e292eb300013c1d6052f3df7bbe228869` (`git -C D:\Projects\p4b-interp-validation rev-parse HEAD`). AudioMan check lines at that HEAD match `c8f8188ae0` (`git grep` output below).
Commands and machine-readable dumps: `collect.py`, `job_inventory.json`, `history_hits.json`, `history_summary.json`, `repeat_compare.json`, `package_sizes.json`, `peers_jobs.json`, `git_c8f_to_9029.txt`, `git_9029_to_c8f_audio.txt`, `git_flake_stat.txt`.

No engine launch. No build. No write outside this directory.

---

## 1. Exact failure sequence on the returner

### 1.1 `drop3_returner\stdout.log` (entire file, 18 lines)

```
1:[audio] output silenced for the headless run
2:libpng warning: iCCP: known incorrect sRGB profile
3:[scenario] pinned dt 0x3c88893b -> 0x3c888865
4:[net-match] state transfer complete: 5409186 bytes
5:[net-match-service-e2e] lobby_snapshot: state=Running is_host=0 members=3 local_ready=1 remote_ready=1 activity=P4 Alpha Duel scene=Grasslands mode=pvp-skirmish | peer1=Host(team0,remote,ready,ping0ms) | peer2=Client(team1,local,ready,ping0ms) | peer3=Client(team2,remote,ready,ping0ms)
6:[snapbench] read_ms=581
7:[net-match] launching from the received snapshot: p5resync_17092_recv
8:[snapbench] restart_ms=148
9:[audio-checkpoint] voice 36 has no registered owner 9310
10:[music-checkpoint] music audio restoration failed
11:[gui-sound-checkpoint] GUI audio restoration failed
12:[runtime-globals] apply failed: could not restore GUI/music/audio checkpoint
13:[runtime-globals] validation failed: invalid AudioMan checkpoint: voice 1 has owner 9124 past the sound container cursor 9067
14:[scriptgraph] reinstate refused before the world moved
15:[runtime-globals] apply failed: invalid AudioMan checkpoint: voice 1 has owner 9124 past the sound container cursor 9067
16:[net-match] could not launch the activity
17:[net-match-service-e2e] wrote report: D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner_report.json
```

Tick/frame on the returner: none. This file has no `[net-match] … tick` / `frame` line. `drop3_returner_report.json` has `running_ticks: 0`, `pace.sim_ticks: 0`, `activity_state: "NoActivity"`, `runtime_error: "could not launch the activity"`, `exit_code: 1`, `service.reconnect.client_commits: 1`, `client_state: "Joined"`. Snapshot receive is line 4 (5409186 bytes) then line 7 (`p5resync_17092_recv`). Apply failure is lines 9–16, still with no tick printed.

### 1.2 `drop3_returner\runtime\LogConsole.txt` (entire file)

```
1:- RTE Lua Console -
2:See the Data Realms Wiki for commands: http://www.datarealms.com/wiki/
3:Press F1 for a list of helpful shortcuts
4:-------------------------------------
5:Sound loading completed: 984 pending samples, 1241ms
6:SYSTEM: Game "p5resync_17092_recv" staged for restart!
7:SYSTEM: Activity was reset!
8:SYSTEM: Scene "Grasslands" was loaded
9:SYSTEM: Activity "P4 Alpha Duel" was successfully started
10:ERROR: Could not restore audio checkpoint: voice 36 has no registered owner 9310
11:ERROR: the prior game's Lua state could not be reinstated
12:ERROR: could not launch the activity
13:ERROR: No Activity to end!
14:SYSTEM: Entire console contents saved to LogConsole.txt
```

Line 9 (`Activity … successfully started`) is `RestartActivityCandidate` succeeding. Line 10 is the apply-time AudioMan throw (same text as stdout:9). Line 11 is `ActivityMan.cpp:1232` after `ReinstateWorld` returned false.

### 1.3 Host, same window (`drop3_host\stdout.log`)

```
7:[net-match] waiting on peer frames (tick 193, Client)
8:[net-match] Client left the match at frame 193 (connection lost)
9:[net-match] peer stall recovered after 4459ms (tick 193)
10:[net-reconnect] reseating team 1 onto peer 2 (2 actors)
11:[net-match] rejoin: Client reconnected - resyncing the match
12:[net-match] resync: requested, reloading from the host snapshot
13:[snapbench] save main_ms=135 zip_io_ms=77 saved=1
14:[snapbench] read_ms=576
15:[net-match] launching from the received snapshot: p5resync_5680
16:[snapbench] restart_ms=140
17:[net-match] resync: match relaunched from the snapshot
18:[net-match] Client left the match at frame 308 (connection lost)
19:[net-match] reseat: team 1 -> peer 2 actors 2/2
```

Host save tick is not printed on the `snapbench save` line. The drop that triggered the save is tick/frame 193. After the host relaunched `p5resync_5680`, the returner disconnect is frame 308 (`drop3_host_report.json` `runner.lockstep.peer_leave_frames["2"]=308`, `configured_start_frame=308`, `handed over 5414746`).

### 1.4 Host `LogConsole.txt`

```
9:NETWORK: Resyncing from the host (1): ResyncRequested:player rejoined
10:SYSTEM: Game saved to "p5resync_5680"!
11:SYSTEM: Game "p5resync_5680" staged for restart!
12:SYSTEM: Activity was reset!
13:SYSTEM: Scene "Grasslands" was loaded
14:SYSTEM: Activity "P4 Alpha Duel" was successfully started
15:SYSTEM: Activity "P4 Alpha Duel" was ended
```

### 1.5 Surviving client (`drop3_client2\stdout.log`)

```
6:[net-match] waiting on peer frames (tick 193, Client)
7:[net-match] Client left the match at frame 193 (connection lost)
8:[net-match] peer stall recovered after 4465ms (tick 193)
9:[net-match] resync: requested, reloading from the host snapshot
10:[net-match] state transfer complete: 5409186 bytes
11:[snapbench] read_ms=584
12:[net-match] launching from the received snapshot: p5resync_21308_recv
13:[snapbench] restart_ms=141
14:[net-match] resync: match relaunched from the snapshot
15:[net-match] Client left the match at frame 308 (connection lost)
16:[net-match] reseat: team 1 -> peer 2 actors 2/2
```

`drop3_client2\runtime\LogConsole.txt`:

```
9:NETWORK: Resyncing from the host (1): tick 308 lockstep wait: ResyncRequested:player rejoined
10:SYSTEM: Game "p5resync_21308_recv" staged for restart!
11:SYSTEM: Activity was reset!
12:SYSTEM: Scene "Grasslands" was loaded
13:SYSTEM: Activity "P4 Alpha Duel" was successfully started
14:SYSTEM: Activity "P4 Alpha Duel" was ended
```

Client2 applied the same 5409186-byte transfer and launched. No `[audio-checkpoint]` / `[runtime-globals]` line on client2.

### 1.6 Dropped client (`drop3_client1\stdout.log`, injected kill)

```
1:[audio] output silenced for the headless run
2:libpng warning: iCCP: known incorrect sRGB profile
3:[scenario] pinned dt 0x3c88893b -> 0x3c888865
4:[net-match-service-e2e] lobby_snapshot: …
5:[snapbench] restart_ms=92
```

`verdict.json` records `drop3_client1.injected_termination="injected mid-match client drop"`, `exit_code=137`, `elapsed_seconds=10.132`. No snapshot/audio lines.

### 1.7 Snapshot / checkpoint / journal files in the job

Walker: `collect.py` → `job_inventory.json`. `os.scandir`, skip symlink / `st_file_attributes & 0x400`, skip named `Data`/`external`/`modules`. 145 non-reparse files. 11 `runtime\Data` reparse points skipped. No file in the job is above 50 MB (largest retained traces ≈830 KB). No `p5resync_*` save file remains.

| name | size | note |
|---|---:|---|
| `drop3_host\runtime\Userdata\UserSavedGames.rte\Index.ini` | 99 | stub only; host console named `p5resync_5680` |
| `drop3_returner\runtime\Userdata\UserSavedGames.rte\Index.ini` | 99 | stub only; returner named `p5resync_17092_recv` |
| `drop3_client2\runtime\Userdata\UserSavedGames.rte\Index.ini` | 99 | stub only; client2 named `p5resync_21308_recv` |
| same `Index.ini` under peers3_* / peers4_* / drop3_client1 (8 more) | 99 each | empty module index |
| `drop3_host_trace.json` | 813556 | sim-hash journal |
| `drop3_client2_trace.json` | 813580 | sim-hash journal |
| `drop3_returner` has no `*_trace.json` | — | returner argv omitted `-out` |
| `peers3_*_trace.json` (3) | 813478–813546 | peers3 arm |
| `peers4_*_trace.json` (4) | 829844–829912 | peers4 arm |
| `drop3_*.ticket` / `peers3_*.ticket` / `peers4_*.ticket` | 155 each | reconnect tickets |
| `verdict.json` | 46100 | gate record |
| `drop3_returner_report.json` | 2103 | returner match report |
| `drop3_host_report.json` | 13863 | host match report |
| `drop3_*_trace.json.console.txt` | 732–742 | console excerpts |

The 5409186-byte snapshot blob is not on disk under this job after the run. Host `snapbench save … saved=1` and `SYSTEM: Game saved to "p5resync_5680"!` are the only save records.

---

## 2. Engine checks that fired

Quoted from `D:\Projects\p4b-interp-validation\Source\` at worktree HEAD `9751a90e29`. Same line numbers on pin `c8f8188ae0` for the two AudioMan strings (`git -C D:\Projects\cccp grep -n "has no registered owner" c8f8188ae0 -- Source/Managers/AudioMan.cpp` and the cursor string).

### 2.1 `has no registered owner` — apply path

`Source/Managers/AudioMan.cpp:1521` inside `AudioMan::LoadCheckpoint` (`1480`), after `AudioRuntime::Load` has already returned true.

```
1519:		for (const auto& voice: state.voices) {
1520:			SoundContainer* owner = voice.owner ? FindCheckpointSoundContainer(voice.owner) : nullptr;
1521:			if (voice.owner && !owner) throw std::runtime_error("voice " + std::to_string(voice.identity) + " has no registered owner " + std::to_string(voice.owner));
```

Catch at `1632–1636` prints `[audio-checkpoint]` and `ERROR: Could not restore audio checkpoint: …`.

Trigger: archived voice has a non-zero owner identity, and `FindCheckpointSoundContainer` (`1069–1071`) returns null (`m_CheckpointSoundContainers` has no entry, or the entry's owner list is empty). Lookup uses `found->second.back()`.

### 2.2 `past the sound container cursor` — validate path

`Source/Managers/AudioMan.cpp:1419` inside `AudioRuntime::Load` (`1400`), used by `AudioMan::LoadCheckpoint` before any apply (`1482–1483`: `if (!state.Load(text, refusal)) return false; if (validateOnly) return true;`).

```
1417:				for (const auto& voice: voices) {
1418:					if (!voiceIDs.insert(voice.identity).second) return refuse("duplicate voice identity " + std::to_string(voice.identity));
1419:					if (voice.owner > nextSoundContainer) return refuse("voice " + std::to_string(voice.identity) + " has owner " + std::to_string(voice.owner) + " past the sound container cursor " + std::to_string(nextSoundContainer));
```

Trigger: a voice's archived owner id is greater than the archived `nextSoundContainer` cursor.

### 2.3 Names: voice, owner, cursor

| term | type | writer | reader / check |
|---|---|---|---|
| voice | `AudioCheckpoint::Voice` (`Source/System/AudioCheckpoint.h:224`) / live `AudioMan::PlayingVoice` (`AudioMan.h:438`) | `SaveCheckpoint` `1470–1473`: `Voice::Capture(identity, owner ? owner->GetCheckpointIdentity() : 0, …)` from `m_PlayingVoices` | `Load` iterates `state.voices`; apply rebuilds `PlayingVoice{channel, owner*, path, …}` |
| owner (archive) | `uint64_t AudioCheckpoint::Voice::owner` (`AudioCheckpoint.h:226`) | SoundContainer checkpoint identity, or 0 if no live owner | validate: compared to cursor; apply: looked up in the sound-container registry |
| owner (live) | `SoundContainer* PlayingVoice::owner` (`AudioMan.h:440`) | set at `RegisterPlayingVoice` (`1110`) | `FindCheckpointSoundContainer` (`1069`) |
| cursor | `AudioRuntime::nextSoundContainer` (`1379`) / live `m_NextSoundContainerIdentity` (`AudioMan.h:450`), exposed as `GetCheckpointSoundContainerCursor` (`AudioMan.h:74`) | `SaveCheckpoint` `1437`: `state.nextSoundContainer = m_NextSoundContainerIdentity` | `Load` field; apply commit `1613` writes it back. IDs come from `AllocateCheckpointSoundContainerID` (`1048`, pre-increment) and `RegisterCheckpointSoundContainer` (`1053`, `max` with identity) |

Writer: `AudioMan::SaveCheckpoint` `1434–1478`. Reader/validator: `AudioRuntime::Load` `1400–1428`. Apply: `AudioMan::LoadCheckpoint` `1480–1637`.

### 2.4 Wrapper strings

| string | path:line | condition |
|---|---|---|
| `music audio restoration failed` | `Source/Managers/MusicMan.cpp:653` | `LoadCheckpointWithAudio` `634`: after swapping music state, `g_AudioMan.LoadCheckpoint(audio, false, &candidate->sampleBindings)` returned false. Catch `655–660` prints `[music-checkpoint]`. |
| `GUI audio restoration failed` | `Source/GUI/GUISound.cpp:282` | `GUISoundCheckpoint::Apply` `247`: after swapping GUI sounds into the registry, `g_MusicMan.LoadCheckpointWithAudio(music, audio, &candidate->bindings)` throws. Catch `284–292` prints `[gui-sound-checkpoint]`. |
| `could not restore GUI/music/audio checkpoint` | `Source/Managers/ActivityMan.cpp:1126` | `RestoreRuntimeGlobals` OnCommit `1123–1127`: `LoadCheckpointWithAudio` / `LoadCheckpoint` returned false. |
| `invalid AudioMan checkpoint: ` | `ActivityMan.cpp:1122` | immediate validate: `!g_AudioMan.LoadCheckpoint(audio, true, nullptr, &audioRefusal)`. |
| `reinstate refused before the world moved` | `Source/Managers/MovableMan.cpp:1535` | `ReinstateWorld` `1529`: `RestoreRuntimeGlobals(in.runtimeGlobals, true)` failed (or terrain `CanRestore` failed). |

### 2.5 `[runtime-globals]` validate vs apply order

`ActivityMan::RestoreRuntimeGlobals` `1073` (`ActivityMan.h:135`). `CheckpointReader` (`CheckpointArchive.h:90`) stores OnCommit lambdas only when `validateOnly` is false (`100`). `Finish` (`95–98`) runs those lambdas after the whole archive parses.

Order for RuntimeGlobals9 (current):

1. Parse RNGs, then `component(...)` for TimerMan / MovableMan / SceneMan / CameraMan / FrameMan: each calls `LoadCheckpoint(state, true)` immediately and queues apply in OnCommit (`1088–1091`).
2. Flags, shared GUI input, UInputMan, PostProcessMan, PrimitiveMan: same validate-now / apply-later.
3. GUISound validate (`1113`), MusicMan validate (`1118`), AudioMan validate (`1120–1122`).
4. OnCommit queued (`1123–1127`): `hasGUI ? g_GUISound.LoadCheckpointWithAudio(gui, music, audio) : …`.
5. `reader.Finish()` runs applies. Catch `1131–1133` prints `[runtime-globals] validation failed` or `apply failed` from `validateOnly`.

This job's stdout order matches two `RestoreRuntimeGlobals` calls after the first apply:

1. `RestartActivity` `1202`: `RestoreRuntimeGlobals(m_PendingCheckpoint.runtimeGlobals)` apply. AudioMan validate of the **received** snapshot passed (no cursor line yet). OnCommit → GUISound → MusicMan → AudioMan apply → owner 9310 missing → stdout 9–12. `CaptureRuntimeGlobals` for the rollback was taken at `1178` before set-aside; `SetAsideWorld` takes another copy at `MovableMan.cpp:1413`.
2. `ReinstateWorld` `1534`: `RestoreRuntimeGlobals(in.runtimeGlobals, true)` on the **set-aside** globals. AudioMan validate fails cursor 9124 > 9067 → stdout 13–14.
3. `RestartActivity` `1230`: `RestoreRuntimeGlobals(oldGlobals)` apply of the **pre-restart** capture. Same cursor refusal at the immediate validate → stdout 15 (still labelled `apply failed` because `validateOnly` is false). Then stdout 16.

So the received snapshot failed apply (missing registry owner 9310). The returner's own captured AudioMan text then failed validate (voice 1 owner 9124 past cursor 9067).

---

## 3. Prior fix (`stage2/audio-checkpoint-flake` tip `9029cb3d64`)

Repo: `git -C D:\Projects\cccp`. Tip: `9029cb3d6495541281e573803a687d86e1c63cb5` (`git name-rev` → `stage2/audio-checkpoint-flake`).

| command | result | evidence |
|---|---|---|
| `git log --oneline c8f8188ae0..9029cb3d64` | empty | `git_c8f_to_9029.txt` |
| `git log --oneline 9029cb3d64..c8f8188ae0 -- Source/Managers/AudioMan.cpp Source/Managers/AudioMan.h Source/System/AudioCheckpoint.h Source/GUI/GUISound.cpp Source/Managers/MusicMan.cpp` | empty | `git_9029_to_c8f_audio.txt` |
| `git merge-base --is-ancestor 9029cb3d64 c8f8188ae0` | exit 0 (yes, ancestor) | merge-base printed `9029cb3d6495541281e573803a687d86e1c63cb5` |
| `git -C D:\Projects\p4b-interp-validation rev-parse HEAD` | `9751a90e29…` | worktree moved past the pin |

Branch-own commits that touch the audio checkpoint / archive (from `git log --stat --format=fuller 7a9e735e76^..9029cb3d64 -- Source/Managers/AudioMan.cpp Source/Managers/AudioMan.h Source/System/CheckpointArchive.h`, also `git_flake_stat.txt`):

1. `7a9e735e76` **Name the value a runtime checkpoint refusal came from** — `AudioMan.cpp +28/−13`, `AudioMan.h +1/−1`. `AudioRuntime::Load` gained a `refusal` string. The pre-existing silent `voice.owner > nextSoundContainer` became the named `past the sound container cursor` text. `LoadCheckpoint` gained `std::string* refusal`. The apply-time `has no registered owner` throw was not in this diff.
2. `55c5f9d908` **Cover the audio setting a boot never assigns** — `AudioMan.cpp +15`. Selftest: refuse a `MuteAudioOnFocusLoss` byte that is not 0/1, and require the manager's own value to be a serialisable bool.
3. `96ccf1ef0e` **Initialise the audio manager's unset members** — `AudioMan.cpp +6`. `Clear()` now zeros the five FMOD handles and `m_MuteAudioOnFocusLoss = false`.
4. `56b73a9c08` **Merge the audio manager initialisation fix: the checkpoint flake was an unset member, not an FMOD race** — merge `60359be83e` + `96ccf1ef0e`. Also threads `invalid AudioMan checkpoint: ` + `audioRefusal` in `ActivityMan.cpp:1122` (`git diff 60359be83e 56b73a9c08 -- Source/Managers/ActivityMan.cpp`).
5. `d1c29203f0` **Write a bool's storage byte, not the compiler's opinion of it** — `CheckpointArchive.h +11/−1`. Non-template bool writer uses `memcpy` of the storage byte.
6. `9029cb3d64` **Pin the archive text an unset setting produces** — `AudioMan.cpp +17/−1`. Selftest pins archive text `13 AudioRuntime3 0 0 0 0 0 0 100 ` for a poisoned `muteOnFocusLoss` byte.

What that fix changed (from those diffs, not inference): it initialized `MuteAudioOnFocusLoss` and the FMOD handle pointers in `Clear()`, made bool archive writes copy the storage byte, named AudioRuntime validation refusals (including the cursor check's message), threaded the refusal into `RestoreRuntimeGlobals`, and added selftests that a non-0/1 mute byte is refused and that the writer emits that byte. It did not add or remove `has no registered owner`. That string was introduced by `261edc3771` **feat(snapshot): restore native audio voices and owner controls** (`git log -S "has no registered owner" --all`; first engine hit). The cursor **condition** `voice.owner > nextSoundContainer` already existed as a bare `return false` before `7a9e735e76`; that commit only named it. `9029cb3d64..c8f8188ae0` has no later AudioMan/GUISound/MusicMan/AudioCheckpoint commits, so HEAD still has those checks as left by the flake branch.

---

## 4. History of this failure class

Scanner: `collect.py` (`os.scandir`, skip symlink/reparse/`Data`/`external`/`modules`; `*.log`/`*.txt`/`*.json` under 20 MB in `D:\mx\s41b3`, `D:\mx\s41`, `D:\mx\s40lanes`; `D:\Projects\reviews\**\*.md` plus `D:\Projects\RESUME.md`, lines truncated to 400). Output: `history_hits.json`, `history_summary.json`, `peers_jobs.json`.

### 4.1 Runtime / matrix hits

| file | line | job/gate/arm | peer | message |
|---|---:|---|---|---|
| `D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner\stdout.log` | 9 | `peers_3_4_regression` / j40 / 20260910_022527 | drop3_returner | `[audio-checkpoint] voice 36 has no registered owner 9310` |
| same | 13 | same | drop3_returner | `validation failed: … voice 1 has owner 9124 past the sound container cursor 9067` |
| same | 15 | same | drop3_returner | `apply failed: … voice 1 has owner 9124 past the sound container cursor 9067` |
| `…\drop3_returner\runtime\LogConsole.txt` | 10 | same | drop3_returner | `ERROR: Could not restore audio checkpoint: voice 36 has no registered owner 9310` |
| `…\verdict.json` | 940, 942, 945 | same | (gate record) | same cursor strings copied into `verdict_lines` / `last_verdict` |
| `…\drop3_returner\launch.json` | 89, 91, 94 | same | drop3_returner | same copies |
| `D:\mx\s41b3\breadth.json` | 9108, 9111–9113 | Source41 breadth v3 index of this job | drop3_returner | copies of the four log lines |
| `D:\mx\s41\shutdown\graph\stdout.log` | 141 | s41 arm `shutdown/graph` | graph | `[audio-checkpoint] voice 1 has no registered owner 9156` |
| `D:\mx\s41\shutdown\graph\runtime\LogConsole.txt` | 7 | same | graph | `ERROR: Could not restore audio checkpoint: voice 1 has no registered owner 9156` |
| `D:\mx\s41\native_graph\stdout.log` | 141 | s41 arm `native_graph` | native_graph | `[audio-checkpoint] voice 1 has no registered owner 9156` |
| `D:\mx\s41\native_graph\runtime\LogConsole.txt` | 7 | same | native_graph | same ERROR line |
| `D:\mx\s40lanes\**` | — | scanned 2339 files, skipped_large=6 | — | **0 hits** |

Repeats 1, 2, 4, 5 under `j38`/`j39`/`j41`/`j42` have no needle hits.

### 4.2 Review docs and `RESUME.md`

`RESUME.md:103` (only exact-needle hit; truncated to 400 around the match): `…fails deterministically at capture 338 with \`[audio-checkpoint] voice 40 has no registered owner 9293\` thrown from \`AudioMan::LoadCheckpoint\`…` — the older fuzz-buy class, not this j40 residual. No `past the sound container cursor` in `RESUME.md`.

Review-doc hits (37 lines, `history_hits.json`): this residual is already quoted in `reviews\takeover-20260909\grok-workers\w2-breadth-triage\LEAD_VERDICTS.md:9` and `TRIAGE.md:257–283`. Earlier class occurrences in docs: `rollback-audio-owner` (`voice 40 has no registered owner 9293`, `voice 1 … 9081` / `9098`), `breadth-debt` / `fuzz-buy-audio-checkpoint-red.md` (same 9293 from Source19 onward), `mac-native-triage` (`voice 1 … 9069` / `9081`, several marked as selftest negative controls). `source33-lanes\report.md:2231` records `has no registered owner` count **0** for that older lane.

### 4.3 `peers_3_4_regression` run counts

| root | jobs | this class in a returner/engine log |
|---|---:|---:|
| `D:\mx\s41b3` | 5 (`j38`…`j42`) | 1 (`j40` repeat 3) |
| `D:\mx\s41` | 0 | 0 |
| `D:\mx\s40lanes` | 5 (`h4\peers_3_4_regression_20260909_105714` … `_110551`) | 0 |

Total `peers_3_4_regression` jobs across those roots: **10**. Jobs whose engine logs carry this class: **1**. The five s40lanes jobs are `verdict.passed=false` on `no_census_refusals` (`census_refusals=1` in `…105714\verdict.json:78–80`); they do not carry these two strings. s41 `shutdown/graph` and `native_graph` are a separate gate family with the owner-missing string only.

---

## 5. Repeat 3 vs passing repeats 1, 2, 4, 5

Sources: the five job `stdout.log` / `LogConsole.txt` files and `repeat_compare.json`. Same exe `bb3cf264…`, same `built_from=c8f8188ae0`.

| | repeat 1 `j38` `…022228` | repeat 2 `j39` `…022357` | **repeat 3 `j40` `…022527`** | repeat 4 `j41` `…022655` | repeat 5 `j42` `…022824` |
|---|---|---|---|---|---|
| gate `verdict.passed` | true | true | true | true | true |
| returner `exit_code` / `runtime_error` | 0 / empty | 0 / empty | **1 / could not launch the activity** | 0 / empty | 0 / empty |
| returner `running_ticks` | 601 | 601 | **0** | 601 | 601 |
| returner event order after lobby | transfer → read → launch `p5resync_19116_recv` → restart → **reseat** | transfer → read → launch `p5resync_5520_recv` → restart → **reseat** | transfer → read → launch `p5resync_17092_recv` → restart → **audio-checkpoint → music → gui → apply failed → validation failed → reinstate refused → apply failed → could not launch** | transfer → read → launch `p5resync_6112_recv` → restart → **reseat** | transfer → read → launch `p5resync_1224_recv` → restart → **reseat** |
| returner snapshot tick printed | none | none | none | none | none |
| returner apply/reseat tick printed | none (reseat has no tick) | none | none (apply has no tick) | none | none |
| returner transfer bytes | 5476307 | 5467438 | **5409186** | 5467780 | 5477079 |
| returner `[audio-checkpoint]` / `[audio]` besides silence | `[audio] output silenced…` only | same | silence **plus** stdout 9–15 | silence only | silence only |
| returner LogConsole after start | activity ended | (same pattern) | **audio ERROR + Lua reinstate ERROR + could not launch** | activity ended | activity ended |
| host drop tick/frame | 198 | 198 | **193** | 200 | 200 |
| host `snapbench save` | main_ms=136 zip_io_ms=77 saved=1 | 135 / 77 / 1 | 135 / 77 / 1 | 137 / 78 / 1 | 134 / 78 / 1 |
| host save name | `p5resync_676` | `p5resync_13660` | `p5resync_5680` | `p5resync_5192` | `p5resync_9160` |
| host post-resync | `resync: match relaunched` then **reseat** (no second leave) | same | relaunched then **Client left at frame 308** then reseat | reseat, no second leave | reseat, no second leave |
| host `resyncs` | 1 | 1 | 1 | 1 | 1 |
| host `[audio-checkpoint]` | none | none | none | none | none |
| client2 transfer / launch | 5476307 / `p5resync_2320_recv` / relaunched | (same pattern) | 5409186 / `p5resync_21308_recv` / relaunched, then leave at 308 | (same pattern) | (same pattern) |

Facts that differ on repeat 3: smaller transfer (5409186 vs 5.467–5.477 MB), earlier drop tick (193 vs 198/200), returner apply sequence present, returner exit 1 / 0 ticks, host/client2 second leave at frame 308 after the returner failed to launch. Save timings are the same order of magnitude. Passing returners have no `[audio-checkpoint]` line.

---

## 6. Reproduction package list (retain before compaction)

Sizes from `os.stat(follow_symlinks=False)` → `package_sizes.json` and `job_inventory.json`. PDB listed by size only (153 MB; not opened).

| path | size | role |
|---|---:|---|
| `D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\Cortex Command.exe` | 18177536 | pinned exe `bb3cf264…` |
| `D:\Projects\p4b-interp-validation\Cortex Command.exe` | 18177536 | approved-tree copy; same byte size; family launched this path |
| `D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\Cortex Command.pdb` | 153251840 | symbols (stat only) |
| `D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json` | 1542 | build manifest (`head=c8f8188ae0`) |
| `D:\mx\s41b3\pin.json` | 384 | family pin |
| `D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\h4gates\peers_3_4_regression.py` | 9725 | gate driver |
| `D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\h4gates\common.py` | 8900 | driver helper |
| `D:\mx\s41b3\plan.json` | 48452 | repeat mapping (`j38`–`j42`) |
| `D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\verdict.json` | 46100 | gate record (`passed: true`, returner `exit_code: 1`) |
| `…\drop3_returner\stdout.log` | 1316 | failing sequence |
| `…\drop3_returner\runtime\LogConsole.txt` | 673 | console ERRORs |
| `…\drop3_returner\launch.json` | 3836 | runner record |
| `…\drop3_returner_report.json` | 2103 | `could not launch the activity` |
| `…\drop3_host\stdout.log` | 1430 | save / tick 193 / leave 308 |
| `…\drop3_host\runtime\LogConsole.txt` | 685 | `Game saved to "p5resync_5680"` |
| `…\drop3_host\launch.json` | 3228 | host runner record |
| `…\drop3_host_report.json` | 13863 | `resyncs=1`, `start_frame=308` |
| `…\drop3_host_trace.json` | 813556 | host sim journal |
| `…\drop3_client2\stdout.log` | 1331 | survivor applied same 5409186-byte snap |
| `…\drop3_client2_report.json` | 7971 | survivor report |
| `…\drop3_client2_trace.json` | 813580 | survivor sim journal |
| `…\drop3_client1\stdout.log` | 460 | injected drop |
| `…\drop3_client1.ticket` | 155 | ticket the returner reused |
| passing controls: `j38`/`j39`/`j41`/`j42` `drop3_returner\stdout.log` + `drop3_host\stdout.log` | 823–1430 | clean event order |

Fixture: in-engine `P4 Alpha Duel` / `Grasslands` / `setup_surface=fixed-alpha-duel` via `-net-match-service-e2e` (`Source/Main.cpp:184`, `3479`; driver `peers_3_4_regression.py`). No separate fixture file on disk. The named resync saves (`p5resync_5680`, `p5resync_17092_recv`) are **not** in the job tree; only the transfer size and console names remain.

---

## Commands run (read-only)

- Read returner/host/client logs and `verdict.json` / `pin.json` / `plan.json` / reports
- `rg` in `D:\Projects\p4b-interp-validation\Source` for the six literal strings
- `git -C D:\Projects\cccp log/show/merge-base/grep` as section 3
- `python -B collect.py` (reparse-safe walk; wrote the JSON dumps in this directory)
