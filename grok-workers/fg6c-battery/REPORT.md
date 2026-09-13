# FG6C-BATTERY acceptance (run-only evidence)

Stamp 2026-09-13 12:10 MST. This is the worker evidence report, not a lead verdict. `collect_report.py` draft classifications are not used as proof.

## Deviations / not-done FIRST

1. `remaining_steps.py:861` always `return 0`. `remaining_exit.txt=0` and `run_all.py` exit 0 do not mean remaining rows passed. `inventory-exits.json` `run_all.py` exit 0 is that launcher return. `run_n6.py` exit 0 is the N6 checks only.
2. Guard `child_exit=0` / `stop_reason=child_exit` is the inventory process exiting, not a test pass.
3. No quiet reruns. `ALLOW_TIMING_RERUNS=False`. `fencing_h4_quiet_rerun` recorded as skipped, exit 0, reason `automatic rerun disabled; original result retained for lead classification`. Not a fencing pass.
4. `LEAD_BATTERY.lock` present for this lane (FG6C, HEAD `7e63a0c1b52983d9461113fcf883d14b1f970e50`, EXE `c67b35a65b05…`, 2026-09-13 09:58:15 MST). Never deleted. `LEAD_EXCLUSIVE.lock` absent. `remaining_steps.wait_for_clear` / `run_battery.wait_for_clear` wait only on `LEAD_EXCLUSIVE.lock`. Inherited `tools/wait_engine_idle.py`, `w102-fixtures-takeover-e2e/wait_engine.py`, and `tools/test_directory_ice_join.py:wait_engine_free` wait for `LEAD_BATTERY.lock` to vanish. Those three tools never reached an engine launch.
5. Rows that never launched an engine: `arm1` (empty `arm1.log`, no `D:\mx\fg6bat3\arm1`), `ak47hud_1/2/3` (no `D:\mx\fg6bat3\ak47hud`), `ice_e2e` (directory service only; no host/client/verdict). Exit `4294967295` (`0xFFFFFFFF` / -1). No `.dmp`, no `AbortLog.txt`, no FATAL in those logs.
6. `compare_overlap` exit 2: `returner_trace=null` after remaining `reclaim` fail. Comparison not executed.
7. `controller_log` scored missing fixture (no `-controller-log` result file).
8. Resync-frame table: `scan_resync.py` / `trace_scan.py` exist in the lane; `remaining_steps.py` never calls them. `trace_scan.json` absent. Row NOT RUN.
9. Named standalone native arms `ownership` / `path-update` / `relay-stop` / `pie-delay` / `sound-identity` are not separate inventory rows. Battery ran `-script-graph-selftest` only. Those exact PASS tokens were not found in `D:\mx\fg6bat3\selftests\native-graph\stdout.log`. Verify6 graph186 / localUI39 / localState10 was not rerun (brief).
10. `present_identity.py` preserved as `present_identity.py.before` SHA256 `60F5AD4336313E2AAA4107231BFC7A1F9613BD567D7B1725867E9C7C966B4488`. After `set_present_ticks()` the file is byte-identical (`TICKS = 180` both sides). `present_ticks.json` `{"ticks": 180, "trace": "D:\\mx\\fg6bat3\\lobby\\early\\host\\trace.json"}`. Run traces have 181 hashes; comparator `compared_ticks=0`.
11. N6 desktop-gate: `isolated_launch.py --hold` from 2026-09-13 11:30:09 MST until host launch `started_utc` 2026-09-13T19:05:55.177797+00:00 (12:05:55 MST), about 35.8 min. Then N6 engines ran and passed. No `CC_RUNNER_IGNORE_FULLSCREEN`.
12. `stalls.json` never written. No `LEAD_EXCLUSIVE` wait recorded by the battery waiters.
13. Extra lane files written for execution only: `preflight_check.ps1`, `watch_inventory.ps1`, `watch_progress.py`, `watch_n6.ps1`, `watch_n6_children.ps1`, `watch_n6_tree.ps1`, `extract_evidence.py`, `postflight_check.ps1`. Drivers/pins/brief not edited.
14. `collect_report.py` header still says FG6B; several of its baseline cells are wrong (global-callback “flipped green”, fencing_two_transports “flipped red”, heal_wp “flipped red”). Replaced below from raw files.
15. No dumps under `D:\mx\fg6bat3\dumps`. No leftover `Cortex Command` / inventory python after 12:09:30 MST.

## Identity / pin

- tree `D:\Projects\control-build` branch `stage2/fixgroup-6-lead`
- HEAD `7e63a0c1b52983d9461113fcf883d14b1f970e50` (before and after)
- exe `D:\Projects\control-build\Cortex Command.exe` SHA256 `c67b35a65b05b0dfb4794a432048eb0b62e6a999bc9be6c8221fd7087784b504`
- build 2026-09-13 09:22:40 MST; log `D:\mx\lead-candidate-20260913\build.log` SHA256 `0f5c97065f51adfcd4296ca9438748e2c778c33db882c25254193b8bfb17298e`
- Source excluding CLI: 610 files; `exe_newer_than_source=true`; compile source clean; firewall inbound true
- identity-before `2026-09-13 10:00:49 MST` `identity_ok=true` all 10 checks true; identity-after `2026-09-13 12:06:43 MST` same
- manifest `D:\mx\fg6bat3\build-manifest.json` SHA256 `e7372cecf4d164271ffe25dd871ea21e952d3179b984bc0ed65283fb7bd87a29`
- `LEAD_BATTERY.lock` true both identities; `LEAD_EXCLUSIVE.lock` false both

## Guard / scratch

```
guard-result.json: start 2026-09-13 10:01:34 MST, end 2026-09-13 12:06:33 MST
timeout=24000 interval=2 maxbytes=5368709120
stop_reason=child_exit child_exit=0 peak_bytes=1093238230 openssl_present=true
cmd=python .../run_inventory.py
```

Peak 1093238230 regular-file bytes (~1.02 GiB), under 5 GiB cap. No cap/timeout stop.

## Driver differences (this run)

- Prepared copies already in `diffs/` (59 audited; not recopied).
- `final-execution-policy`: B1 pin is this exe; automatic timing reruns disabled.
- Disclosed `present_identity` TICKS rewrite: before/after identical, `TICKS = 180`, `present_ticks.json` as above.
- No other driver, oracle, mask, or fixture edit.

## N1–N6 commands (as recorded)

N1 each i=1..3:

`python D:\Projects\control-build\tools\run_ak47_replay.py --repo D:\Projects\control-build --out D:\mx\fg6bat3\ak47hud\run{i} --timeout 900 --replay D:\Projects\stage2_p4\fixtures\ak47_fire.ccreplay --script D:\Projects\stage2_p4\fixtures\ak47_fire.txt --max-ticks 600 -- -local-prediction-depth 7 -local-prediction-hud 500`

N2: `python D:\Projects\reviews\takeover-20260909\grok-workers\fg6c-battery\w119_h4gates\peers_3_4_regression.py --arm expire3 --port-expire 47661`

N3: `python D:\Projects\control-build\tools\test_directory_ice_join.py --out D:\mx\fg6bat3\ice-e2e --port 8458 --game-port 47666 --ticks 600 --cert D:\mx\w75\cert.pem --key D:\mx\w75\key.pem`

N4: `python D:\Projects\control-build\tools\w97_port_map_e2e.py --repo D:/Projects/control-build --run-root D:/mx/fg6bat3/portmap`

N5 drop3: `python ...\w102_h4gates\peers_3_4_regression.py --arm drop3 --port-drop 47663`; lpinv and controller_log as remaining_steps.

N6: `pwsh -NoProfile -File ...\run_interp_e2e.ps1 -Repo D:\Projects\control-build -SkipBuild -ReplayTest -InputDelay 3 -Port 47666 -DeliverCommandHost -Ticks 700 -OutDir D:\mx\fg6bat3\n6\craft_cargo -ExactOutDir` then unchanged `check_fixture.py craft_cargo ... --delay 3` then `w71-4-path-update-after-inflight\analyze_dumps.py`.

## Pass/fail table

Baseline column is vs FG6B `fg6b-battery\REPORT.md` (context only). Observed is this run.

| name | observed | last raw line / note | out | baseline vs FG6B |
|---|---|---|---|---|
| net-match | pass runner=0 engine=0 | `[net-match-selftest] PASS` | `D:\mx\fg6bat3\selftests\net-match` | yes |
| net-lockstep | pass runner=0 engine=0 | `[net-lockstep-selftest] PASS` | `...\net-lockstep` | yes |
| net-reconnect-session | pass runner=0 engine=0 | `[net-reconnect-session-selftest] PASS` | `...\net-reconnect-session` | yes |
| net-session | pass runner=0 engine=0 | `[net-session-selftest] PASS` | `...\net-session` | yes |
| net-admission | pass runner=0 engine=0 | `[net-admission-selftest] PASS` | `...\net-admission` | yes |
| net-auth | pass runner=0 engine=0 | `[net-auth-selftest] PASS` | `...\net-auth` | yes |
| net-protocol | pass runner=0 engine=0 | `[net-protocol-selftest] PASS` | `...\net-protocol` | yes |
| net-identity | pass runner=0 engine=0 | `[net-identity-selftest] PASS` | `...\net-identity` | yes |
| net-discovery | pass runner=0 engine=0 | `[net-discovery-selftest] PASS` | `...\net-discovery` | yes |
| net-reconnect | pass runner=0 engine=0 | `[net-reconnect-selftest] PASS` | `...\net-reconnect` | flipped green |
| controller-frame | pass runner=0 engine=0 | `[controller-frame-selftest] PASS` | `...\controller-frame` | yes |
| net-directory | pass runner=0 engine=0 | `[net-directory-selftest] PASS` | `...\net-directory` | yes |
| net-port-map | pass runner=0 engine=0 | `[net-port-map-selftest] PASS` | `...\net-port-map` | yes (FG6B new) |
| net-p2p-gather | pass runner=0 engine=0 | `[net-p2p-selftest] PASS` | `...\net-p2p-gather` | yes |
| global-callback | fail/refuse runner=1 engine=1 | `[global-callback-selftest] REFUSE needs -net-replay ...` `stdout.log:3` | `...\global-callback` | yes |
| native-graph | pass runner=0 engine=0 | last `[script-graph-selftest] PASS refused_reinstate_leaves_the_next_hold_possible`; 0 FAIL lines; `coroutine_near_cstack_limit_8011` PASS; local-state 10 PASS; local-ui retained/fresh/seatless + inventory-brain PASS | `...\native-graph` | yes |
| native-graph-s1 | pass runner=0 engine=0 | same last PASS | `...\native-graph-s1` | yes (FG6B new) |
| native-graph-diag-on | pass runner=0 engine=0 | same last PASS | `...\native-graph-diag-on` | yes |
| gns_provider_smoke | pass exit=0 | `GATE gns_provider_smoke PASS` | `...\h4\gns_provider_smoke_20260913_170518` | yes |
| old_wire_fixture-inprocess | pass exit=0 | `GATE old_wire_fixture PASS` | `...\old_wire_fixture_20260913_170545` | yes |
| clean_leave | pass exit=0 | `GATE clean_leave PASS` | `...\clean_leave_20260913_170606` | yes |
| reclaim_socket (H4 battery) | pass exit=0 | `GATE reclaim_socket PASS` | `...\reclaim_socket_20260913_170720` | yes |
| crash_relaunch_provisional | pass exit=0 | `GATE crash_relaunch_provisional PASS` | `...\crash_relaunch_provisional_20260913_170753` | yes |
| fencing_two_transports (old h4gates) | fail exit=1 | `FAIL host_fenced_the_old_transport fenced_disconnects+fenced_packets=0` | `...\fencing_two_transports_20260913_170913` | yes |
| rejoin_after_resync | pass exit=0 | `GATE rejoin_after_resync PASS` | `...\rejoin_after_resync_20260913_170932` | yes |
| peers_3_4_regression | pass exit=0 | `GATE peers_3_4_regression PASS` drop3_survivors_exit_zero; seats host=1,client2=3,returner=2 | `...\peers_3_4_regression_20260913_171037` | yes |
| substitute_commit | pass exit=0 | GATE PASS | `...\b1\substitute_commit\...` | yes |
| substitute_returner_wins | pass exit=0 | GATE PASS | `...\substitute_returner_wins\...` | yes |
| substitute_host_cancel | pass exit=0 | GATE PASS | `...\substitute_host_cancel\...` | yes |
| substitute_bounds | fail exit=1 | `two_applicants_registered` fail detail=1; `third_applicant_refused` fail detail=0 | `...\substitute_bounds_20260913_171642\verdict.json` | flipped red |
| lobby-early | pass exit=0 | `result.pass=true` | `D:\mx\fg6bat3\lobby\early` | yes |
| lobby-rejoin | pass exit=0 | `result.pass=true` | `D:\mx\fg6bat3\lobby\rejoin` | yes |
| gcb | pass exit=0 | `{"pass": true, ...}` | `D:\mx\fg6bat3\gcb` | yes (FG6B new) |
| suite | pass exit=0 | `passed=11 total=11` exe `c67b35a65b05…` | `D:\mx\fg6bat3\suite\result.json` | flipped green |
| fencing_h4_quiet_rerun | skipped | reruns disabled; not a pass | remaining_progress | new skip |
| heal | pass exit=0 | `resync_heal` pass; `result.pass=true` | `D:\mx\fg6bat3\heal\result.json` | yes |
| heal_wp | fail exit=1 | `failed=["host_script_continued","client_script_continued"]`; `waypoints_match_across_peers` pass | `D:\mx\fg6bat3\heal_wp\result.json` | yes |
| fencing_1 | pass exit=0 | `GATE fencing_two_transports PASS` fenced=1 | `fencing_1.log` / `...\h4\fencing_two_transports_20260913_172310` | yes |
| fencing_2 | pass exit=0 | GATE PASS | `fencing_2.log` | yes |
| fencing_3 | pass exit=0 | GATE PASS | `fencing_3.log` | yes |
| fencing_4 | pass exit=0 | GATE PASS | `fencing_4.log` | yes |
| fencing_5 | pass exit=0 | GATE PASS | `fencing_5.log` | yes |
| reclaim (remaining h4gates) | fail exit=1 | see FAIL block | `reclaim.log` / `...\reclaim\reclaim_socket_20260913_172624` | flipped red |
| compare_overlap | fail exit=2 | `host or returner trace not found`; host_trace present; returner_trace null | remaining_progress | flipped red |
| fixtures | pass exit=0 | `GAMEPLAY FIXTURES: PASS (15 checks)`; `binary_matches_source` PASS | `fixtures.log` / `D:\mx\fg6bat3\gameplay` | flipped green |
| crab_ai_order | pass exit=0 | `GAMEPLAY FIXTURES: PASS (3 checks)`; `binary_matches_source` PASS | `crab_ai_order.log` | flipped green |
| dedicated_ded3 | pass exit=0 | GATE dedicated_host PASS | `dedicated_ded3.log` | yes |
| dedicated_drop | pass exit=0 | GATE dedicated_host PASS | `dedicated_drop.log` | yes |
| P3COOP | pass exit=0 | `GATE lifecycle_P3COOP_a1 PASS` | `p3coop.log` | yes |
| P4PVPVE | pass exit=0 | `GATE lifecycle_P4PVPVE_a1 PASS`; `PASS p4_resumed_at_drop_frame` drop_frame=255 expected=256 resumed_start all 256 | `p4pvpve.log` | yes |
| probe_right | pass engine=0 | `[net-directory-probe] PASS` | `probe_right.log` / `D:\mx\fg6bat3\probe_right` | yes |
| probe_wrong | fail engine=1 expected | `[net-directory-probe] POST /v1/sessions -> status=0 error=certificate pin mismatch body=` | `D:\mx\fg6bat3\probe_wrong\stdout.log:4` | yes |
| probe_signal | pass engine=0 | `[net-directory-signal-probe] PASS` | `probe_signal.log` | yes |
| session_directory tests | pass exit=0 | `Ran 29 tests in 38.455s` / `OK` | `session_directory_tests.log:1278-1280` | yes |
| directory_e2e | pass exit=0 | both arms host/client exit 0; enabled url=`127.0.0.1:8458` | `directory_e2e.log` | yes |
| boot | pass exit=0 | `settings_unchanged=true`; 174 lines; staged/after text sha `c1a19b747968…`; no install key | `boot.log` / `D:\mx\fg6bat3\boot` | yes |
| present_identity | fail exit=1 | TICKS=180; traces 181; `compared_ticks=0`; sha_match_tip_tip2 true | `present_identity.log` / `D:\mx\fg6bat3\present` | yes |
| lobby_rejection | pass exit=0 | pass | `lobby_rejection.log` | yes |
| lobby_input_delay | pass exit=0 | pass | `lobby_input_delay.log` | yes |
| fl100 | pass exit=0 | `fl100 PASS []` compared_ticks=600 | `fl100.log` | yes |
| mixed_lua | pass exit=0 | exit 0 | `mixed_lua.log` / `D:\mx\fg6bat3\mixed` | yes |
| heal_global_d0 | pass exit=0 | exit 0 | `heal_global_d0.log` | yes |
| heal_global_d3 | pass exit=0 | exit 0 | `heal_global_d3.log` | yes |
| arm1 | fail exit=4294967295 | no engine; empty log; no scratch dir | `arm1.log` | flipped red |
| drop3_switch | pass exit=0 | `GATE peers_3_4_regression PASS`; `drop3_returner_owner_log_resumes` 520 | `drop3_switch.log` | yes (FG6B new) |
| preview_event_d7 | pass exit=0 | press tick 153; W104b projectile PASSes; `preview_codec_fallback=0` | `preview_event_d7.log` / `...\preview-event-d7\stdout.log:27` | yes (FG6B new) |
| lpinv_100 | pass exit=0 | `[lpinv] PASS tick 100: 8/8 cases passed invariance and link checks`; `preview_codec_fallback=0` | `lpinv_100.log` | yes (FG6B new) |
| portmap_e2e | pass exit=0 | ports 8469/47601/8467 not busy; exit 0 | `portmap_e2e.log` | yes (FG6B new) |
| controller_log | fail exit=1 | missing result file; no `-controller-log` fixture | remaining_progress | yes |
| ak47hud_1 | fail exit=4294967295 | only `waiting locks=['D:\\mx\\LEAD_BATTERY.lock']`; no trace; no dir | `ak47hud_1.log` | flipped red |
| ak47hud_2 | fail exit=4294967295 | same lock wait; 1s | `ak47hud_2.log` | flipped red |
| ak47hud_3 | fail exit=4294967295 | same lock wait; 1s | `ak47hud_3.log` | flipped red |
| ak47hud_compare | fail exit=1 | `missing traces`; all sha256 None | `ak47hud_compare.log` | flipped red |
| expire3 | pass exit=0 | GATE PASS; checker `PASS claimed_actor_expiry` later=512 uid 1048634 | `expire3.log` / `expire3_checker.log` | yes (FG6B new) |
| ice_e2e | fail exit=4294967295 | 16× `[ice-e2e] waiting for the shared engine`; service listened 127.0.0.1:8458; no host/client/verdict | `ice_e2e.log` / `D:\mx\fg6bat3\ice-e2e` | flipped red |
| n6 | pass exit=0 | pulses both `[316, 317, 318, 319]`; uid 1048840; semantic/e2e PASS; no AIOrder reject in pulse.log | `D:\mx\fg6bat3\n6\result.json` | new |
| resync-frame table | NOT RUN | `scan_resync.py` never invoked | — | missing |

Gameplay pack `craft_cargo` (fixtures row) is separate from N6.

## FAIL checks verbatim

Pin for every row: HEAD `7e63a0c1b52983d9461113fcf883d14b1f970e50` exe `c67b35a65b05b0dfb4794a432048eb0b62e6a999bc9be6c8221fd7087784b504`.

### global-callback

Cmd: `python tools\run_sim_test.py --repo D:\Projects\control-build --out D:\mx\fg6bat3\selftests\global-callback --timeout 30 -- -global-callback-selftest`

`D:\mx\fg6bat3\selftests\global-callback\stdout.log:3`

```
[global-callback-selftest] REFUSE needs -net-replay <recording> or -scenario, and UserScenes.rte Checkpoint Global
```

gcb driver form passed separately.

### fencing_two_transports (old h4gates)

Cmd: `python ...\fg6c-battery\h4gates\fencing_two_transports.py` `CC_H4_RUN_ROOT=D:\mx\fg6bat3\h4`

`progress.json` stdout / gate dir `D:\mx\fg6bat3\h4\fencing_two_transports_20260913_170913`:

```
  FAIL host_fenced_the_old_transport fenced_disconnects+fenced_packets=0
GATE fencing_two_transports FAIL -> D:\mx\fg6bat3\h4\fencing_two_transports_20260913_170913
```

fencing_1..5 through `fencing_h4gates` all GATE PASS (`fenced_disconnects+fenced_packets=1` on fencing_1.log:4).

### substitute_bounds

Cmd: `python D:\Projects\control-build\tools\h4_substitution_gates.py substitute_bounds --out D:\mx\fg6bat3\b1\substitute_bounds --expect-sha256 c67b35a65b05b0dfb4794a432048eb0b62e6a999bc9be6c8221fd7087784b504 --repo D:\Projects\control-build`

`D:\mx\fg6bat3\b1\substitute_bounds\substitute_bounds_20260913_171642\verdict.json`:

```
"name": "two_applicants_registered", "status": "fail", "detail": "1"
"name": "third_applicant_refused", "status": "fail", "detail": "0"
```

`passed: false`. Host/stayer exit 0; applicants exit 1; leaver exit 137. `bound_respected` pass detail=0; `nothing_committed` pass.

### heal_wp

Cmd: `python ...\heal_wp_run.py D:\mx\fg6bat3\heal_wp --ai-write-script`

`heal_wp_stdout.txt:1-8` and `D:\mx\fg6bat3\heal_wp\result.json` `pass: false`:

```
resync_heal PASS []
{"case": "heal_mod_semantics", "pass": false, "failed": ["host_script_continued", "client_script_continued"]}
```

`host_script_continued` status=fail rows start `1,26,26,0` (`result.json:429`). `waypoints_match_across_peers` status=pass uid 1048634 count=498 (`result.json:28027`).

### reclaim (remaining)

Cmd: `python ...\fg6c-battery\h4gates\reclaim_socket.py` `CC_H4_RUN_ROOT=D:/mx/fg6bat3/reclaim`

`reclaim.log:1-20`:

```
  PASS executable_matches_manifest c67b35a65b05b0dfb4794a432048eb0b62e6a999bc9be6c8221fd7087784b504
  PASS host_exit_zero 0
  PASS first_client_dropped_midmatch terminated by the driver
  FAIL returner_reclaimed_stored_ticket
  FAIL returner_committed
  FAIL host_saw_seat_drop_and_return 0 committed seats
  PASS returner_on_same_stable_seat shared ticket path
  PASS host_census_clean 0
  FAIL host_reseat_issued no reseat decision; printed=False; host_seats_dropped=1, host_ledger_drops_recorded=1, host_reseats_issued=0, ...
  FAIL resync_round_survived
  FAIL match_continued_after_reclaim effective_start=1 completed=248 reason='PeerLeft:connection lost' activity_state=Over
  FAIL resume_frame_is_drop_frame_plus_one leave_frame=249 resume=250 starts={'host': (1, 1), 'returner': (0, 0)}
  FAIL snapshot_taken_at_the_leave_frame leave_frame=249 resync={}
  FAIL resumed_round_is_contiguous_on_both_peers host: start=1 completed=248 ...; returner: start=0 completed=None ...
  FAIL host_simulated_every_frame_once pace.sim_ticks=248 leave_frame=249 frames_accepted=248
  PASS no_undelivered_session_events session_events={'discarded': 0, 'drained_at_teardown': 0}
  FAIL returner_played exit_code=1 setup_error='session timeout (timeout_ms: 5000 vs 5001)' runtime_error='' frames_accepted=0 running_ticks=0
  PASS funds_unchanged no funds command was issued in this gate
  PASS no_authority_error
GATE reclaim_socket FAIL -> D:\mx\fg6bat3\reclaim\reclaim_socket_20260913_172624
```

H4 battery `reclaim_socket` on the same driver copy earlier the same day was GATE PASS (`...\h4\reclaim_socket_20260913_170720`).

### compare_overlap

Not launched. `remaining_progress.json` `error=host or returner trace not found` `host_trace=D:\mx\fg6bat3\reclaim\reclaim_socket_20260913_172624\host_trace.json` `returner_trace=null`.

### present_identity

Cmd: `python ...\present_identity.py D:\mx\fg6bat3\present`

`present_identity.log` / summary: tip/tip2/ref each ticks=181, sha `8b775e70…`, `tip_vs_tip2.identical=false`, reasons `host: has 181 ticks, expected exactly 180` and same for client. `compared_ticks=0`.

### arm1

Cmd: `python D:\Projects\reviews\takeover-20260909\grok-workers\w102-fixtures-takeover-e2e\run_arm1.py --repo D:\Projects\control-build --out D:\mx\fg6bat3\arm1 --port 47661`

`arm1.log` empty. `D:\mx\fg6bat3\arm1` missing. `w102 wait_engine.py` waits on `LEAD_BATTERY.lock` (2 h). remaining timeout 2400s. exit `4294967295`. No dump.

### controller_log

`remaining_progress.json`: `exit_code=1` `note=missing result file: no -controller-log fixture; step removed from the battery baseline greens`.

### ak47hud_1 / _2 / _3 / compare

Cmds: N1 above. `tools\wait_engine_idle.py` waits while `LEAD_BATTERY.lock` exists (default 7200s). remaining timeout 960s.

`ak47hud_1.log` (17 lines, ~8 min wall 18:13:23–18:21:24 UTC):

```
waiting locks=['D:\\mx\\LEAD_BATTERY.lock'] pids=[]
```

`ak47hud_2.log` / `ak47hud_3.log`: one wait line each, ~1 s, same exit. `D:\mx\fg6bat3\ak47hud` missing. `ak47hud_compare.log`:

```
sha256 D:\mx\fg6bat3\ak47hud\run1\trace.json=None
sha256 D:\mx\fg6bat3\ak47hud\run2\trace.json=None
sha256 D:\mx\fg6bat3\ak47hud\run3\trace.json=None
missing traces
```

No `[preview-hud-selftest]`, no dump, no FATAL.

### ice_e2e

Cmd: N3 above. `test_directory_ice_join.py` starts directory then `wait_engine_free()` on `LEAD_BATTERY.lock`. remaining timeout 2400s. ~8 min (18:22:14–18:30:08 UTC), 16 wait lines, exit `4294967295`.

`ice_e2e.log`:

```
[ice-e2e] waiting for the shared engine
```

`D:\mx\fg6bat3\ice-e2e\service.log`:

```
2026-09-13 11:22:14,486 INFO listening on 127.0.0.1:8458 tls=True
2026-09-13 11:22:14,889 INFO 127.0.0.1 "GET /v1/sessions HTTP/1.1" 200 -
```

No `verdict.json`, no host/client traces.

## N6 raw

`D:\mx\fg6bat3\n6\result.json` `pass=true` all four checks true. `pulse.log`:

```
uid 1048840 common_ticks=331
host_wp_nonzero [316, 317, 318, 319]
client_wp_nonzero [316, 317, 318, 319]
```

`driver.log` RESULT PASS 18 checks; host/client exit 0; running_ticks 701; sim-gated 700 ticks. `semantic.log` craft_cargo PASS peers identical 27..640. Host launch `exe_sha256` matches pin; `CCCP_HEADLESS=1`; private desktop; firewall check true.

## Settings / provenance

Boot: settings_unchanged true; 174 lines; text sha `c1a19b747968afe072d6c1d5c6488a6d440e8b96a0dc9449256ddaa56abd3894`. Heal/N6 match config hash `8c5c9bd8637f18753f39d6032aecde167705ea33895b5456f33b2798f5b6d3ff`. Directory pin `02b2e52f00a9ef7ff3da2f078be7bf597000521b2cb92337eb29715e38151bde`; wrong pin `82b2e52f…`.

## Missing-row inventory

| required inherited row | status |
|---|---|
| resync-frame table (`session_events.discarded` / `service.directory.state` scan) | NOT RUN; `scan_resync.py` never called |
| verify6 graph186 / localUI39 / localState10 as a batch | not rerun (brief) |
| named ownership/path-update/relay-stop/pie-delay/sound-identity standalone rows | not in `run_battery`/`remaining_steps`; tokens not in native-graph log |
| fencing_h4_quiet_rerun as a real second fencing | skipped |
| compare_overlap comparison | not executed (no returner trace) |
| arm1 engine + checker | not launched |
| AK HUD engine ×3 + serialized traces | not launched |
| ice_e2e engines / verdict | not launched |

All other inherited W81-4 / FG6 / FG6B rows above were invoked and have an observed result.

## Files for the lead

- `D:\Projects\reviews\takeover-20260909\grok-workers\fg6c-battery\REPORT.md` (this file)
- `identity-before.json` `identity-after.json` `identity.json` `build-pin.json`
- `guard-result.json` `inventory-exits.json` `progress.json` `remaining_progress.json`
- `present_identity.py.before` `present_identity.py` `present_ticks.json` `present_identity.log`
- `reclaim.log` `heal_wp_stdout.txt` `arm1.log` `ak47hud_1.log` `ak47hud_2.log` `ak47hud_3.log` `ak47hud_compare.log` `ice_e2e.log`
- `D:\mx\fg6bat3\build-manifest.json`
- `D:\mx\fg6bat3\b1\substitute_bounds\substitute_bounds_20260913_171642\verdict.json`
- `D:\mx\fg6bat3\reclaim\reclaim_socket_20260913_172624`
- `D:\mx\fg6bat3\h4\fencing_two_transports_20260913_170913`
- `D:\mx\fg6bat3\suite\result.json`
- `D:\mx\fg6bat3\n6\result.json` `pulse.log` `semantic.log` `driver.log`
- `D:\mx\LEAD_BATTERY.lock` (do not delete)
