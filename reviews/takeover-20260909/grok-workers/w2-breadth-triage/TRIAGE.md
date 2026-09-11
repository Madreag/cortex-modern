# Source41 Windows breadth v3 — 13 non-pass triage

Read-only on `D:\mx\s41b3` and every repository. This file is the only intended deliverable.

Pin from `D:\mx\s41b3\pin.json` and `D:\mx\s41b3\breadth.json`: source 41, head `c8f8188ae0`, exe `bb3cf264b4e7304f45486440998e4b63e37d5d46d0d119caa04f76c2fc378ffb`, 81/81 collected, `passed=false`, 13 non-pass. Classifications below are proposals; the lead decides.

Command that listed the 13 ids: `python D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage\extract_nonpass.py` → `nonpass_cases.json`.

---

## 1. `fl200`

1. Job `D:\mx\s41b3\j28`. Family `fakelag`. Driver: `D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\run_fakelag.py fl200` (`plan.json` id `fl200`; wraps unmodified `D:\Projects\stage2_p4\recovery_e2e.py`). Breadth validator family `fakelag` in `D:\Projects\reviews\takeover-20260909\run_breadth.py`.

2. The e2e lane itself passed. Oracle that failed the breadth case is the wrapper, not `result.json`:

```
D:\mx\s41b3\breadth.json:2047: host: auto delay differs from 14
D:\mx\s41b3\breadth.json:2048: client: auto delay differs from 14
D:\mx\s41b3\logs\fl200.log:3: fl200 PASS []
D:\mx\s41b3\j28\fl\fl200\result.json:442: "pass": true
```

Measured values (both peers, lockstep reports are single-line JSON):

```
D:\mx\s41b3\j28\fl\fl200\host\stdout.log:5: [net-match] auto input delay: peer 2 rtt 400ms -> 13 frames (manual floor 0)
host_report.json peer_input_delays = {"1": 1, "2": 13}; start_packets_sent=2; start_retransmits=1; resyncs=0
client_report.json peer_input_delays = {"1": 1, "2": 13}; start_packets_sent=2; start_retransmits=1; resyncs=0
```

Same-family control `fl100` (`D:\mx\s41b3\j27`) passed: `host\stdout.log:5` `rtt 201ms -> 8 frames`.

Expectation `14` is encoded only in the breadth wrapper, not in `tools/` and not in `run_fakelag.py`:

```
D:\Projects\reviews\takeover-20260909\run_breadth.py:1665-1666
delay = 8 if job.meta["case"] == "fl100" else 14
v.require(ls.get("peer_input_delays") == {"1": 1, "2": delay}, f"{peer}: auto delay differs from {delay}")
```

Derivation uses wall-clock transport ping:

```
D:\Projects\p4b-interp-validation\Source\Network\NetMatchRunner.cpp:79-89
tickMs = 1000.0/30.0
neededDelay = ceil(rttMs / tickMs) + 1
rttMs = transport.GetPeerPingMs(transportId)
```

```
D:\Projects\p4b-interp-validation\Source\Network\GnsTransport.cpp:347-356
SteamNetConnectionRealTimeStatus_t status
return status.m_nPing > 0 ? uint32_t(status.m_nPing) : 0
```

`GetPeerPingMs` is GNS `m_nPing` (wall-clock RTT). Load that moves 401 ms → 400 ms crosses `ceil(rtt/tickMs)`: `ceil(400/33.333…)+1 = 13`, `ceil(401/33.333…)+1 = 14`. Fake-lag 200 ms each way is the ~400 ms RTT the formula is scoring.

3. Source40 baseline (passed):

```
D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\report.md:1645
fl200: auto input delay: peer 2 rtt 401ms -> 14 frames  (unchanged)
report.md:1992
auto delays back to **8 and 14**
report.md:2260
resyncs 0 and the auto delays back at 8 and 14
```

Source39 documented the same formula crossing a frame on **fl100**, not fl200:

```
report.md:1931
the only moved number is fake-lag's fl100 auto-delay, 8 → **7**, which is §4a's formula crossing a frame boundary because the measured RTT came in at 200 ms rather than 201 ms, not a code change
```

Residue is not identical: Source40 measured 401→14 and passed; Source41 measured 400→13 and the wrapper failed. The e2e checks (600-tick identity, 0 resyncs, start chatter 2/1) still passed.

4. Proposed: `SOURCE41_REGRESSION_SUSPECT` versus the Source40 pass, with the measured mechanism being the documented RTT-boundary formula (Source39 fl100). Evidence: wrapper pin 14 at `run_breadth.py:1666`; host log 400→13; Source40 401→14. Widening the pin to accept 13 would hide a real delay-code change if one appeared later.

---

## 2. `h4_clean_leave_1`

1. Job `D:\mx\s41b3\j33`. Driver: `...\driver\h4gates\clean_leave.py`. Repeat 1.

2. Gate `verdict.json` `passed=false`. Failing checks:

```
D:\mx\s41b3\j33\clean_leave_20260910_021812\verdict.json:40-42
"name": "clean_old_ticket_refused", "status": "fail", "detail": "used_stored_ticket=True commits=1"
verdict.json:64-66
"name": "hosts_survived", "status": "fail", "detail": "exits=[0, 0, 1] fatal=False"
```

Host (ambiguous arm) console, no tick on the ERROR line. Host stdout places the later leave at frame/tick 249:

```
D:\mx\s41b3\j33\clean_leave_20260910_021812\ambiguous_host\runtime\LogConsole.txt:9
ERROR: Rejected a AIOrder command from a peer that does not control team 1
ambiguous_host\stdout.log:7-9
[net-match] waiting on peer frames (tick 249, Client)
[net-match] Client left the match at frame 249 (connection lost)
[net-match] peer stall recovered after 4447ms (tick 249)
```

The ERROR sits after `Activity "P4 Alpha Duel" was successfully started` and before `Holding the match open` (LogConsole.txt:8-10). Client logs in this job do not contain `Rejected a`.

Passing checks on this run that Source40 had red: `clean_leave_acked`, `clean_leave_cleared_ticket`, `host_closed_the_seat`.

3. Source40 (lanes table) and Source40 retained log (identical AIOrder line):

```
report.md:2014
`clean_leave` | **FAIL** — PASS `ambiguous_kept_ticket`, **PASS `ambiguous_reclaim_worked used_stored_ticket=True commits=1`**; FAIL `clean_leave_acked 0`, `clean_leave_cleared_ticket`, `host_closed_the_seat 0`, `clean_old_ticket_refused used_stored_ticket=True commits=1`, `hosts_survived exits=[0,0,1] fatal=False`
D:\mx\s40lanes\h4\clean_leave_20260909_104531\ambiguous_host\runtime\LogConsole.txt:9
ERROR: Rejected a AIOrder command from a peer that does not control team 1
```

Source38 (older check set, clean arm still unacked): `report.md:1396`. Source39 had the three A6 leave checks green then `hosts_survived exits=[0,0,3]`: `report.md:1669`.

Message shape vs Source40: `clean_old_ticket_refused` and `hosts_survived exits=[0,0,1]` are identical; `clean_leave_acked` / ticket / seat-close flipped to pass; AIOrder line is character-identical to the Source40 retained console.

4. Proposed: `KNOWN_RESIDUE_CHANGED`. Same gate still red; A6 leave checks are green again; AIOrder line is not new (present on Source40 at the same path:line).

---

## 3. `h4_reclaim_socket_1`

1. Job `D:\mx\s41b3\j34`. Driver: `...\driver\h4gates\reclaim_socket.py`.

2.

```
D:\mx\s41b3\j34\reclaim_socket_20260910_021916\verdict.json:16-18
"name": "host_exit_zero", "status": "fail", "detail": "1"
verdict.json:48-50
"name": "host_saw_seat_drop_and_return", "status": "fail", "detail": "0 committed seats"
```

`host_reseat_issued` is **pass** on this run (`verdict.json:72-74`, detail includes `host_reseats_without_survivors=1`).

```
D:\mx\s41b3\j34\reclaim_socket_20260910_021916\host\runtime\LogConsole.txt:9
ERROR: Rejected a AIOrder command from a peer that does not control team 1
host\stdout.log:7-9
tick 250 / frame 250 (connection lost)
```

3. Source40:

```
report.md:2015
`reclaim_socket` | **FAIL** — … FAIL `host_exit_zero 1`, `host_saw_seat_drop_and_return 0 committed seats`, `host_reseat_issued`
D:\mx\s40lanes\h4\reclaim_socket_20260909_104643\host\runtime\LogConsole.txt:9
ERROR: Rejected a AIOrder command from a peer that does not control team 1
```

Source38: `report.md:1397` same three fails including `host_reseat_issued`.

4. Proposed: `KNOWN_RESIDUE_CHANGED`. `host_exit_zero 1` and `0 committed seats` match Source40; `host_reseat_issued` flipped to pass; AIOrder line identical to Source40.

---

## 4. `h4_fencing_two_transports_1`

1. Job `D:\mx\s41b3\j36`. Driver: `...\driver\h4gates\fencing_two_transports.py`.

2. Gate fails (same four check names as Source38–40):

```
verdict.json:24-26 host_bound_second_incarnation fail None
verdict.json:32-34 host_fenced_the_old_transport fail fenced_disconnects+fenced_packets=0
verdict.json:40-42 seat_not_dropped_by_stale_timeout fail []
verdict.json:56-58 host_survived fail 1
```

Client2 (setup) and host:

```
D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\client2\stdout.log:4
[net-match-service-e2e] setup failed: SendMessageToConnection failed with EResult 3 … detail: End-to-end connection: closed by remote host, reason code 1000.  (transport stopped)
host\stdout.log:9
[net-match] resync failed: resync snapshot save failed
host\runtime\LogConsole.txt:11-12
ERROR: Cannot save when there's no game running, or the game is finished!
NETWORK: Resync failed: resync snapshot save failed
```

No `Rejected a` in this job.

3. Source40 check row is the same four fails:

```
report.md:2017
`fencing_two_transports` | **FAIL** — PASS `second_client_committed`, `first_client_superseded`; FAIL `host_bound_second_incarnation None`, `host_fenced_the_old_transport 0`, `seat_not_dropped_by_stale_timeout []`, `host_survived 1`
```

Source38/39 log residue was `GNS peer was not found` (`report.md:1408`, `report.md:1727`). Source40 table does not quote that line; the four check details match.

4. Proposed: `KNOWN_RESIDUE_CHANGED`. Check names/details match Source40; host/client log text is the snapshot-save / remote-close pair, not the older `GNS peer was not found` string.

---

## 5. `h4_rejoin_after_resync_1`

1. Job `D:\mx\s41b3\j37`. Driver: `...\driver\h4gates\rejoin_after_resync.py`.

2.

```
verdict.json:32-34
"name": "resync_match_continued", "status": "fail", "detail": "1"
```

Rematch checks on this run are pass (`rematch_completed`, `rematch_seat_still_protected`, …).

```
D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132\resync_host\runtime\LogConsole.txt:9
ERROR: Rejected a AIOrder command from a peer that does not control team 1
resync_host\stdout.log:7-9
tick 248 / frame 248 (connection lost)
```

3. Source40:

```
report.md:2018
`rejoin_after_resync` | **FAIL** — … FAIL `resync_match_continued 1`, `rematch_completed 124`, `rematch_seat_still_protected []`
D:\mx\s40lanes\h4\rejoin_after_resync_20260909_104844\resync_host\runtime\LogConsole.txt:9
ERROR: Rejected a AIOrder command from a peer that does not control team 1
```

Source38: `report.md:1400` fail `resync_match_continued 1` only (rematch green).

4. Proposed: `KNOWN_RESIDUE_CHANGED`. `resync_match_continued 1` matches Source38 and Source40; rematch fails present on Source40 are green here; AIOrder line identical to Source40.

---

## 6. `h4_peers_3_4_regression_3`

1. Job `D:\mx\s41b3\j40`. Driver: `...\driver\h4gates\peers_3_4_regression.py`. Repeat 3 of 5. Repeats 1,2,4,5 passed (`all_cases.txt`).

2. Gate file says pass. Breadth failed because `scan_logs` matched audio ERROR lines:

```
D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\verdict.json:3
"passed": true
```

All named checks pass, including `no_census_refusals` `census_refusals=0` and `drop3_returner_reclaimed`.

Returner (drop3) only:

```
D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner\stdout.log:9-16
[audio-checkpoint] voice 36 has no registered owner 9310
[music-checkpoint] music audio restoration failed
[gui-sound-checkpoint] GUI audio restoration failed
[runtime-globals] apply failed: could not restore GUI/music/audio checkpoint
[runtime-globals] validation failed: invalid AudioMan checkpoint: voice 1 has owner 9124 past the sound container cursor 9067
[scriptgraph] reinstate refused before the world moved
[runtime-globals] apply failed: invalid AudioMan checkpoint: voice 1 has owner 9124 past the sound container cursor 9067
[net-match] could not launch the activity
drop3_returner\runtime\LogConsole.txt:10-13
ERROR: Could not restore audio checkpoint: voice 36 has no registered owner 9310
ERROR: the prior game's Lua state could not be reinstated
ERROR: could not launch the activity
ERROR: No Activity to end!
```

3. Source40: **0/5**, different residue:

```
report.md:2019
`peers_3_4_regression` | **FAIL ×5/5** — … **`no_census_refusals census_refusals=1`** every run
D:\Projects\reviews\takeover-20260909\source40-evidence-review\report.md:57
`peers_3_4_regression` | 0/5 runs. Every run has `census_refusals=1`
```

Source38/39: **PASS ×5/5** (`report.md:1401`, `report.md:1674`).

4. Proposed: `KNOWN_RESIDUE_CHANGED`. Versus Source40 this repeat is not the census-refusal residue (that counter is 0; gate passed). Versus Source38/39 it is a fail of the audio-checkpoint flake class. The breadth miss is produced by `run_breadth.py:1466-1470` `scan_log` + `ERRORS` matching `ERROR:` / `has no registered owner`.

---

## 7. `b1_substitute_commit`

1. Job `D:\mx\s41b3\j43`. Driver: `D:\Projects\p4b-interp-validation\tools\h4_substitution_gates.py substitute_commit`.

2.

```
D:\mx\s41b3\j43\substitute_commit_20260910_022955\verdict.json:18-60
host_exit_zero fail 1
stayer_exit_zero fail 1
seat_dropped fail None
applicant_registered fail None
substitution_committed fail None
```

Wrapper extras (`breadth.json` / validator after the gate):

```
B1 running activity is not evidenced
B1 did not adjudicate exactly one dropped seat
B1 substitutions_committed is not exactly one
```

Host log (resync **saved**, not save-failed):

```
D:\mx\s41b3\j43\substitute_commit_20260910_022955\host\stdout.log:8-17
Client left the match at frame 993 (connection lost)
[net-reconnect] moderation: substitute seat 1 -> Ok
snapbench save … saved=1
[net-match] resync: match relaunched from the snapshot
```

No `Rejected a` in this job. No seat-drop counter evidenced after relaunch.

3. Source40 lanes table:

```
report.md:2108
`substitute_commit` | **FAIL** | … FAIL `host_exit_zero 1`, `stayer_exit_zero 1`, `seat_dropped None`, `applicant_registered None`, `substitution_committed None`, `reseat_issued`
```

The later Source40 evidence review quotes a **different** residual (`resync snapshot save failed`) at `source40-evidence-review\report.md:61`. This Source41 host log is `saved=1`, matching the lanes Source40 “hosts survive the resync snapshot save” note (`report.md:2262`) and then dying on the activity-over / missing seat evidence.

4. Proposed: `KNOWN_RESIDUE_IDENTICAL` to the Source40 **lanes** check names/details (`host_exit_zero 1`, `stayer_exit_zero 1`, `seat_dropped None`, `applicant_registered None`, `substitution_committed None`). Not identical to the evidence-review’s save-failed wording.

---

## 8. `b1_substitute_returner_wins`

1. Job `D:\mx\s41b3\j44`. Driver: `tools\h4_substitution_gates.py substitute_returner_wins`.

2.

```
verdict.json:18-76
host_exit_zero fail 1
stayer_exit_zero fail 1
seat_dropped fail None
returner_reclaimed fail None
seat_has_one_holder fail None
```

Host: leave at frame 992, `saved=1`, relaunched (`host\stdout.log:8-16`). No `Rejected a`.

3. Source40:

```
report.md:2109
`substitute_returner_wins` | **FAIL** | … FAIL `host_exit_zero 1`, `stayer_exit_zero 1`, `seat_dropped None`, `returner_reclaimed None`, `seat_has_one_holder None`
```

4. Proposed: `KNOWN_RESIDUE_IDENTICAL` to the Source40 lanes check names and details.

---

## 9. `heal`

1. Job `D:\mx\s41b3\j73`. Command: `run_breadth.py --source 41 … --child heal` (`plan.json`).

2. Engine never launched. Full traceback:

```
D:\mx\s41b3\logs\heal.log:1-12
  File "D:\Projects\reviews\takeover-20260909\run_breadth.py", line 1885, in child
    result = module.invariance(out) if options.child == "invariance" else module.heal(out)
  File "D:\Projects\stage2_p4\recovery_expanded_mod.py", line 105, in heal
    import recovery_e2e as e2e
ModuleNotFoundError: No module named 'recovery_e2e'
```

`j73\result.json` does not exist (`breadth.json` records `FileNotFoundError` for that path). No `launch.json` under `j73`.

Defect site: `child()` loads `recovery_expanded_mod.py` and calls `heal()` without putting `D:\Projects\stage2_p4` on `sys.path`. `heal()` does a bare `import recovery_e2e`. The invariance child uses a different file (`recovery_expanded_mod_t300.py`) and passed.

```
run_breadth.py:1879 path = … else STAGE / "recovery_expanded_mod.py"
run_breadth.py:1885 module.heal(out)
recovery_expanded_mod.py:105 import recovery_e2e as e2e
```

Minimal fix (not applied): in `child()`, `sys.path.insert(0, str(STAGE))` before `module.heal(out)`, so the import resolves to `D:\Projects\stage2_p4\recovery_e2e.py`. That cannot hide an engine regression: no engine process started.

3. Source40 heal **PASS**:

```
report.md:2190-2193
### 6. Two-peer heal gate — PASS
`resync_heal PASS []`, `heal_mod_semantics pass: true`
report.md:2264
heal passes with `unresolved_observation_packets` 0
```

Source38/39 also PASS (`report.md:1616`, `report.md:1871`). This Source41 miss is not an engine residue.

4. Proposed: `HARNESS_OR_FIXTURE_DEFECT` at `recovery_expanded_mod.py:105` / `run_breadth.py:1885`.

---

## 10. `compat_deferral_source22`

1. Job `D:\mx\s41b3\j74`. Driver: `...\driver\run_deferral_probe.py source22`.

2. Single wrapper oracle:

```
D:\mx\s41b3\breadth.json:37368
compat output differs from Source22; no UID exclusions
```

Produced by exact list equality:

```
run_breadth.py:1708
v.require(row.get("lines") == reference["lines"], "compat output differs from Source22; no UID exclusions")
```

Pinned line (`run_breadth.py:912`): `PRINT: [deferral] case=spawn_child ok=1 uid=1049517`.

This run (`j74\summary.json` lines, `ok=1` on all 28 cases): `PRINT: [deferral] case=spawn_child ok=1 uid=1049513`.

Every other printed line matches the pin. `cases` all `1`. Same uid `1049513` appears on the approved rung (case 11).

3. Source38 documentation that this uid is run-to-run noise and not build signal:

```
report.md:1531-1534
the `spawn_child` UniqueID counter, `uid=1049494` (reference) against `uid=1049517` (approved), `ok=1` on both. … the reference itself moved: **`source22` printed `1049494` in run 1 and `1049517` in run 2**
report.md:1574
the only line that ever differed was the `spawn_child` UniqueID counter
```

Source39/40: ladder “0 differences” / 28/28 (`report.md:1897-1900`, `report.md:2215`, `report.md:2264`). Those reviews compared rungs to each other and treated the uid as non-signal. This wrapper compares to a frozen uid in `BASELINE["compat"]["deferral"]["lines"]`.

4. Proposed: `HARNESS_OR_FIXTURE_DEFECT` at `run_breadth.py:1708`. The comparison treats the `spawn_child uid` line as signal. A uid-only exclusion would match Source38’s documented finding and would not hide an engine regression **if** every other line and every `ok=1` stay compared (they already match on both rungs). Excluding anything else would.

---

## 11. `compat_deferral_approved`

1. Job `D:\mx\s41b3\j75`. Driver: `run_deferral_probe.py approved`. Same wrapper check `run_breadth.py:1708`.

2. Same oracle string. `j75\summary.json` spawn_child line is `uid=1049513`; all 28 cases `ok=1`; remaining lines match the pin.

3. Same Source38–40 baseline as case 10. Approved and Source22 rungs match each other on this family; both disagree with the frozen `1049517`.

4. Proposed: `HARNESS_OR_FIXTURE_DEFECT` (same site and same uid-only mismatch as case 10).

---

## 12. `compat_extra_source22`

1. Job `D:\mx\s41b3\j80`. Driver: `run_extra_probe.py source22`.

2. PRINT-line comparison against `BASELINE["compat"]["extra"]` is not the recorded failure. `scan_log` recorded the fixture Lua error eight times:

```
D:\mx\s41b3\j80\source22\runtime\LogConsole.txt:12
ERROR: Userdata/UserScenes.rte/compat_review_extra.lua:97: no overload of  'SoundSet:HasAnySounds' matched the arguments (SoundSet)
```

(same text at console.txt:38,43,48 and `trace.json.console.txt` the same four lines).

Fixture call (not in `pcall`):

```
D:\Projects\reviews\claude-review-2026-09-08\lanes\compat-review\fixtures\compat_review_extra.lua:97
local before = s:GetTopLevelSoundSet():HasAnySounds()
```

Binding and C++:

```
D:\Projects\p4b-interp-validation\Source\Lua\LuaBindingsEntities.cpp:1358
.def("HasAnySounds", &SoundSet::HasAnySounds)
SoundSet.h:145 / SoundSet.cpp:357
bool HasAnySounds(bool includeSubSoundSets = true) const
```

Luabind does not apply the C++ default, so a no-arg Lua call is `(SoundSet)` only and misses the `bool` overload. `HasAnySounds` **is** bound. Other SoundSet methods bound in the same block (`LuaBindingsEntities.cpp:1350-1364`): constructor, property `SoundSelectionCycleMode`, readonly `SubSoundSets`, `SelectNextSounds`, `AddSound` (2 overloads), `RemoveSound` (2 overloads), `AddSoundSet`, enum `RANDOM`/`FORWARDS`/`ALL`. `SoundContainer::HasAnySounds` (no-arg) is a separate binding at `:1325`.

`summary.json` cases omit `shared_soundset_structural` (the `say()` after line 97 never runs). That omission is already baked into the Source41 wrapper pin (`run_breadth.py:958-992`), so the PRINT comparison can still match while `scan_log` fails the case.

3. Source38–40 extra: **0 differing PRINT lines**, 14/14 “symmetric” (`report.md:1528`, `report.md:1897`, `report.md:2215`). Those reviews did not treat the console `HasAnySounds` ERROR as a case fail.

4. Proposed: `HARNESS_OR_FIXTURE_DEFECT` at `compat_review_extra.lua:97` (call site) and `LuaBindingsEntities.cpp:1358` (no no-arg overload). Minimal fix: call `HasAnySounds(true)` **or** bind a no-arg wrapper matching the C++ default. Either will start printing `shared_soundset_structural`, which the frozen extra pin does not contain — the pin would need a new both-rung capture, not a silence. Stripping `ERROR:` from `scan_logs` without running the case would leave structural mutation untested and could hide an engine regression.

---

## 13. `compat_extra_approved`

1. Job `D:\mx\s41b3\j81`. Driver: `run_extra_probe.py approved`.

2. Identical `HasAnySounds` ERROR on the approved rung:

```
D:\mx\s41b3\j81\approved\runtime\LogConsole.txt:12
ERROR: Userdata/UserScenes.rte/compat_review_extra.lua:97: no overload of  'SoundSet:HasAnySounds' matched the arguments (SoundSet)
```

Same four repeats in console and `trace.json.console.txt`. PRINT lines match the extra pin (including the missing structural case).

3. Same Source38–40 “0 PRINT differences / both rungs fail the same way” baseline.

4. Proposed: `HARNESS_OR_FIXTURE_DEFECT` (identical to case 12; Source22 and approved fail the same Lua overload).

---

## Lead suspects — extra facts

### `Rejected a` inventory on `D:\mx\s41b3`

Walk of all `j*` (skipped `Data`/`external`/`modules`). Hits in job logs, not counting `breadth.json` echoes:

| job | peer file | line | tick/frame in that peer’s stdout |
|---|---|---|---|
| `j33` clean_leave | `ambiguous_host\runtime\LogConsole.txt:9` | host | leave later at frame/tick **249** |
| `j34` reclaim_socket | `host\runtime\LogConsole.txt:9` | host | leave later at frame/tick **250** |
| `j37` rejoin_after_resync | `resync_host\runtime\LogConsole.txt:9` | host | leave later at frame/tick **248** |

No client/returner/stayer log contains `Rejected a`. Context on all three host consoles is the same five-line window: activity started → ERROR → `Holding the match open` → `Resyncing` → `Game saved`.

Literal `Rejected a` is **not** in `Source\Network\`. The check is:

```
D:\Projects\p4b-interp-validation\Source\Managers\MovableMan.cpp:415-417
} else if (!ScenarioRunner::IsLockstepTeamCommandSender(commandTeam, command.senderPeerId)) {
    g_ConsoleMan.PrintString("ERROR: Rejected a " + … NetGameCommandTypeName(…) … " command from a peer that does not control team " + …);
    continue;
}
```

`git blame -L 404,418 -- Source/Managers/MovableMan.cpp`: print string is `79af0bb826` (2026-06-21, “Only honor an economy command…”). Condition last rewritten to `else if` by `c5fc2fd8bc` (2026-09-08, reseat carve-out). `git log -L 415,417:Source/Managers/MovableMan.cpp` shows only `c5fc2fd8bc`, `363d93a024`, `79af0bb826`.

`9b84ddc7df` (“Reject lockstep packets without a bound transport owner”) is **not** in that history. `git show --stat 9b84ddc7df`: `Source/Network/NetLockstep.cpp` and `NetLockstepSelfTest.cpp` only. The production hunk changes `Handle` transport-owner: absent mapping used to pass (`it == end || bound`); now absent mapping fails (`it != end && bound`). It does not add or edit the MovableMan print.

The same AIOrder console line is already on the Source40 retained H4 hosts (`D:\mx\s40lanes\h4\...\LogConsole.txt:9`, three paths listed in case sections). It is not a Source41-only line.

### Delay measurement and machine load

Yes: auto-delay is `ceil(GNS m_nPing / (1000/30)) + 1`. `m_nPing` is wall-clock. Fake-lag plus host load moves the integer across a 1 ms boundary (400 vs 401). Source39 already recorded that class on fl100.

---

## Summary

| case | classification | one-line reason |
|---|---|---|
| fl200 | SOURCE41_REGRESSION_SUSPECT | S40 401ms→14 pass; S41 400ms→13; wrapper pins 14; e2e lane passed |
| h4_clean_leave_1 | KNOWN_RESIDUE_CHANGED | S40 ticket/ack red; here those pass; `clean_old_ticket_refused` + `hosts_survived [0,0,1]` same; AIOrder also on S40 |
| h4_reclaim_socket_1 | KNOWN_RESIDUE_CHANGED | `host_exit_zero 1` + `0 committed seats` same as S40; `host_reseat_issued` now pass; AIOrder on S40 |
| h4_fencing_two_transports_1 | KNOWN_RESIDUE_CHANGED | same four S40 check fails; logs are remote-close + snapshot-save-failed, not `GNS peer was not found` |
| h4_rejoin_after_resync_1 | KNOWN_RESIDUE_CHANGED | `resync_match_continued 1` same; S40 rematch reds are green; AIOrder on S40 |
| h4_peers_3_4_regression_3 | KNOWN_RESIDUE_CHANGED | gate `passed=true`, census 0; fail is audio-checkpoint flake; S40 was 0/5 `census_refusals=1` |
| b1_substitute_commit | KNOWN_RESIDUE_IDENTICAL | same S40 lanes fails (`exit 1`, `seat_dropped None`, …); resync `saved=1` |
| b1_substitute_returner_wins | KNOWN_RESIDUE_IDENTICAL | same S40 lanes fails (`returner_reclaimed None`, `seat_has_one_holder None`) |
| heal | HARNESS_OR_FIXTURE_DEFECT | `import recovery_e2e` with STAGE not on `sys.path`; no engine |
| compat_deferral_source22 | HARNESS_OR_FIXTURE_DEFECT | only `spawn_child` uid 1049513≠1049517; S38 documented run-to-run; 28/28 `ok=1` |
| compat_deferral_approved | HARNESS_OR_FIXTURE_DEFECT | same uid-only miss; both rungs printed 1049513 |
| compat_extra_source22 | HARNESS_OR_FIXTURE_DEFECT | `HasAnySounds()` no-arg vs bound `(bool)`; S38–40 extra PRINT compare was 0-diff |
| compat_extra_approved | HARNESS_OR_FIXTURE_DEFECT | identical fixture ERROR on approved rung |

---

## Raw files read

- `D:\Projects\RESUME.md` (RECOVERY HANDOFF line 65)
- `D:\mx\s41b3\breadth.json`, `plan.json`, `drivers.json`, `pin.json`, `logs\*.log` for the 13 ids
- Per-job: `j28` fl200 `result.json` / `host_report.json` / `client_report.json` / `host\stdout.log`; `j27` fl100 host stdout; `j33`/`j34`/`j36`/`j37`/`j40`/`j43`/`j44` verdicts + named host/client/returner logs; `j74`/`j75`/`j80`/`j81` summaries + extra consoles; `j73` heal.log
- `D:\mx\s40lanes\h4\clean_leave_20260909_104531\ambiguous_host\runtime\LogConsole.txt`, `reclaim_socket_20260909_104643\host\runtime\LogConsole.txt`, `rejoin_after_resync_20260909_104844\resync_host\runtime\LogConsole.txt`
- `D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\report.md` sections Source38–40
- `D:\Projects\reviews\takeover-20260909\source40-evidence-review\report.md`
- `D:\Projects\reviews\takeover-20260909\run_breadth.py`
- `D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\run_fakelag.py`
- `D:\Projects\stage2_p4\recovery_expanded_mod.py`
- `D:\Projects\reviews\claude-review-2026-09-08\lanes\compat-review\fixtures\compat_review_extra.lua`
- `D:\Projects\p4b-interp-validation\Source\Managers\MovableMan.cpp`, `Source\Network\NetMatchRunner.cpp`, `Source\Network\GnsTransport.cpp`, `Source\Lua\LuaBindingsEntities.cpp`, `Source\Entities\SoundSet.h`, `Source\Entities\SoundSet.cpp`
- git: `blame -L 404,418 Source/Managers/MovableMan.cpp`; `log -L 415,417:Source/Managers/MovableMan.cpp`; `show --stat` and `show` of `9b84ddc7df`

Scratch under this folder: `extract_*.py`, `gather_job_evidence.py`, `nonpass_cases.json`, `rejected_a_hits.json`, `quotes\`, `baseline_source38.txt` / `39` / `40`, `quote_line_numbers.txt`. No repository or `D:\mx` tree was written, moved, or launched.
