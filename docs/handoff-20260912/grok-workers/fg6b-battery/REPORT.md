# FG6B-BATTERY acceptance (run-only)

- executable: `D:\Projects\control-build\Cortex Command.exe`
- branch: `stage2/fixgroup-6-lead`
- start identity (`identity.json`, utc `2026-09-12T21:57:35.961016+00:00`): `git -C D:\Projects\control-build rev-parse HEAD` = `7d3666aa15b0f49b9301610a86be19f8fe704737`; measured exe sha256 = `98f73d74aaed385710ac077d9c0e7e331549ba23a19059b044c6b5f1b6dbe631`; both matched the brief
- `exe_newer_than_source` at start: `true` (exe mtime `2026-09-12T21:52:40.402447+00:00`; newest Source except CLI = `Network\NetReconnectSelfTest.cpp` at `2026-09-12T21:48:17.811598+00:00`; `newer_than_exe=[]`)
- `merge-base --is-ancestor 79c293e1c7 HEAD` true; `8246ea2498` true
- `firewall_allows_inbound` true; `LEAD_EXCLUSIVE.lock` absent at identity
- manifest: `D:\mx\fg6bat2\build-manifest.json` (written from the start identity; `CC_H4_BUILD_MANIFEST` pointed at it)
- mid-battery identity recheck (`identity_recheck.json`, utc `2026-09-12T23:39:52.651227+00:00`): HEAD moved to `5c2c65c2ed0a11591945c467dd280f3ef66ebecd` (`Pin the reclaim proof known answers to protocol version 1`); exe sha256 unchanged; `Source/Network/NetReconnectSelfTest.cpp` mtime `2026-09-12T22:49:32.884987+00:00` newer than the exe. This lane did not edit `control-build`. See FAIL `binary_matches_source` below.
- identity: `D:\Projects\reviews\takeover-20260909\grok-workers\fg6b-battery\identity.json`
- progress: `progress.json` started_utc=`2026-09-12T21:58:51.794298+00:00` finished_utc=`2026-09-12T22:55:41.620536+00:00`
- remaining: `remaining_progress.json` started_utc=`2026-09-12T22:55:41.679931+00:00` finished_utc=`2026-09-12T23:48:13.190593+00:00`
- battery stdout: `battery_all_stdout.txt`; remaining stdout: `remaining_stdout.txt`
- diffs in the lane: `run_battery.diff`, `remaining_steps.diff`, `lead_w75_e2e.diff`, `run_boot.diff`, `heal_e2e.diff`, `check_fixture.diff`, `peers_3_4_regression.diff`, `w85_e2e.diff`, `recovery_expanded_mod.diff`, `run_fakelag_lane.diff`, `lifecycle_pack.diff`, `present_identity.diff`, `test_gameplay_fixtures.diff`, `test_gameplay_fixtures_crab.diff`, `wait_clear.diff`, `w102_peers_3_4.diff`, `w119_peers_3_4.diff`, `w119_common.diff`
- scratch size (no reparse follow): 0.828 GB (`scratch_size.py`; bytes=889235112 files=2767)
- no GUI; every engine launch through `tools/run_sim_test.py` / copied drivers / `win32_test_runner.py`
- loopback directory service: port 8457; game ports 47651-47656 (portmap_e2e used the copied w97 driver defaults 8469 / 47603; no `-net-port-map-probe`)
- `CC_TEST_CRASH_DUMP` under `D:\mx\fg6bat2\dumps\<step>-<n>.dmp`; dumps directory empty after the battery (no `.dmp`); no `AbortLog.txt` under `D:\mx\fg6bat2`
- baseline: FG6-BATTERY partial (`fg6-battery\remaining_progress.json`: fencing_h4_quiet_rerun through P4PVPVE) + W81-4 / W81-4b for everything else
- H4 gates from lane `h4gates` (ports 47651-47656); fencing_1..5 from `fencing_h4gates`; expire3 from `w119_h4gates`; drop3_switch from `w102_h4gates`

## Oracle / stimulus changes (WORKER_RULES)

1. Driver copies from `fg6-battery`: LANE → this lane; scratch `D:\mx\fg6bat` → `D:\mx\fg6bat2`; EXPECT_SHA `84e0285c8a81…` → `98f73d74aaed…`; REPO `fencing-warm` → `control-build` where that string remained; probe/service port → 8457; game ports → 47651-47656. W119 `peers_3_4_regression.py` copied into `w119_h4gates` with expire ports remapped to 47651. Diffs listed above.
2. `remaining_steps.py` additions (declared): N1 AK-47 HUD ×3 + `compare_sim_traces.py`; N2 expire3 + `check_claimed_actor_expiry.py`; N3 `test_directory_ice_join.py` on 8457 / 47656 with `D:\mx\w75\cert.pem` + `key.pem`.
3. `collect_selftests.py` needles extended for W104b / W119 / W123 / W126 / W120 / W130 lines. Report-only.
4. `present_identity` TICKS=180 (same as W81-4). Traces have 181 hashes. `compared_ticks=0`. No second TICKS run.
5. No checker, mask, fixture, or comparison-exclusion edit between a failing run and a passing run.
6. After fixtures printed stale-binary, identity was rechecked and the start `identity.json` / `build-manifest.json` were left as the start-of-battery values (exe sha256 never changed). Documented in `identity_recheck.json`.

`D:\Projects\stage2_p4` has no `-controller-log` fixtures; `control-build\tools` has none. controller_log = missing result file.

## Pass/fail table

| name | exit | last verdict line | output dir | FAIL checks | same as the baseline (fg6 partial / W81-4b)? (yes / flipped green / flipped red / new) |
|---|---|---|---|---|---|
| net-match | runner=0 engine=0 | `[net-match-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-match` | | yes (new mux lines; overall PASS) |
| net-lockstep | runner=0 engine=0 | `[net-lockstep-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-lockstep` | | yes |
| net-reconnect-session | runner=0 engine=0 | `[net-reconnect-session-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-reconnect-session` | | yes |
| net-session | runner=0 engine=0 | `[net-session-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-session` | | yes (new v2 lines) |
| net-admission | runner=0 engine=0 | `[net-admission-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-admission` | | yes |
| net-auth | runner=0 engine=0 | `[net-auth-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-auth` | | yes |
| net-protocol | runner=0 engine=0 | `[net-protocol-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-protocol` | | yes (new digest/chat/old-wire lines) |
| net-identity | runner=0 engine=0 | `[net-identity-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-identity` | | yes (new diff_modules line) |
| net-discovery | runner=0 engine=0 | `[net-discovery-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-discovery` | | yes |
| net-reconnect | runner=1 engine=1 | `[net-reconnect-selftest] FAIL: reclaim proof known-answer mismatch: 56527b38d2db7c0d08f6eb8a9ef61bf8d30ca76b31021009b20b1701cba13b9a` | `D:\mx\fg6bat2\selftests\net-reconnect` | reclaim proof known-answer | flipped red |
| controller-frame | runner=0 engine=0 | `[controller-frame-selftest] PASS` | `D:\mx\fg6bat2\selftests\controller-frame` | | yes |
| net-directory | runner=0 engine=0 | `[net-directory-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-directory` | | yes (new ice-row + pagination lines) |
| net-port-map | runner=0 engine=0 | `[net-port-map-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-port-map` | | new |
| net-p2p-gather | runner=0 engine=0 | `[net-p2p-selftest] PASS` | `D:\mx\fg6bat2\selftests\net-p2p-gather` | | yes |
| global-callback | runner=1 engine=1 | `[global-callback-selftest] REFUSE needs -net-replay <recording> or -scenario, and UserScenes.rte Checkpoint Global` (2.007s, not timed out) | `D:\mx\fg6bat2\selftests\global-callback` | no `[global-callback-selftest] PASS` | yes (still no PASS; W81-4 was timeout 180s/124) |
| native-graph | runner=0 engine=0 | `[script-graph-selftest] PASS refused_reinstate_leaves_the_next_hold_possible` | `D:\mx\fg6bat2\selftests\native-graph` | | yes |
| native-graph-s1 | runner=0 engine=0 | `[script-graph-selftest] PASS refused_reinstate_leaves_the_next_hold_possible` | `D:\mx\fg6bat2\selftests\native-graph-s1` | | new |
| native-graph-diag-on | runner=0 engine=0 | `[script-graph-selftest] PASS refused_reinstate_leaves_the_next_hold_possible` | `D:\mx\fg6bat2\selftests\native-graph-diag-on` | | yes |
| gns_provider_smoke | 0 | `GATE gns_provider_smoke PASS -> D:\mx\fg6bat2\h4\gns_provider_smoke_20260912_220019` | `D:\mx\fg6bat2\h4\gns_provider_smoke_20260912_220019` | | yes |
| old_wire_fixture-inprocess | 0 | `GATE old_wire_fixture PASS -> D:\mx\fg6bat2\h4\old_wire_fixture_20260912_220043` | `D:\mx\fg6bat2\h4\old_wire_fixture_20260912_220043` | | yes |
| clean_leave | 0 | `GATE clean_leave PASS -> D:\mx\fg6bat2\h4\clean_leave_20260912_220105` | `D:\mx\fg6bat2\h4\clean_leave_20260912_220105` | | yes |
| reclaim_socket (lane h4gates) | 0 | `GATE reclaim_socket PASS` | `D:\mx\fg6bat2\h4` (battery arm) | | yes |
| crash_relaunch_provisional | 0 | `GATE crash_relaunch_provisional PASS -> D:\mx\fg6bat2\h4\crash_relaunch_provisional_20260912_220254` | `D:\mx\fg6bat2\h4\crash_relaunch_provisional_20260912_220254` | | yes |
| fencing_two_transports (h4gates old copy) | 1 | `FAIL host_fenced_the_old_transport fenced=0`; GATE FAIL `...\fencing_two_transports_20260912_220413` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_220413` | `host_fenced_the_old_transport` | yes (known a) |
| fencing_two_transports quiet-rerun | 1 | same `fenced=0`; GATE FAIL `...\fencing_two_transports_20260912_223255` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_223255` | `host_fenced_the_old_transport` | (re-run) |
| rejoin_after_resync | 0 | `GATE rejoin_after_resync PASS` | `D:\mx\fg6bat2\h4\rejoin_after_resync_20260912_223633` | | yes |
| peers_3_4_regression | 0 | `GATE peers_3_4_regression PASS` (drop3_survivors_exit_zero; drop3_distinct_seats seats host=1,client2=3,returner=2) | `D:\mx\fg6bat2\h4` | | yes |
| substitute_commit | 0 | GATE PASS | `D:\mx\fg6bat2\b1\substitute_commit` | | yes |
| substitute_returner_wins | 0 | GATE PASS | `D:\mx\fg6bat2\b1\substitute_returner_wins` | | yes |
| substitute_host_cancel | 0 | GATE PASS | `D:\mx\fg6bat2\b1\substitute_host_cancel` | | yes |
| substitute_bounds | 0 | GATE PASS | `D:\mx\fg6bat2\b1\substitute_bounds` | | yes |
| lobby-early | 0 | `result.pass=true` | `D:\mx\fg6bat2\lobby\early` | | yes |
| lobby-rejoin | 0 | `result.pass=true` | `D:\mx\fg6bat2\lobby\rejoin` | | yes |
| gcb | 0 | `{"pass": true, "checks": {exit,callbacks,single_start,cleanup,no_errors,desktop: true}}` | `D:\mx\fg6bat2\gcb` | | new |
| suite | 1 | `passed=10 total=11`; net-reconnect FAIL known-answer `56527b38…` | `D:\mx\fg6bat2\suite` | net-reconnect | new; expected flip was 11/11 |
| fencing_h4_quiet_rerun | 1 | `FAIL host_fenced_the_old_transport fenced=0` GATE FAIL `...\230200` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_230200` | `second_client_committed`; `host_bound_second_incarnation`; `host_fenced_the_old_transport`; `first_client_superseded` | yes (known a) |
| fencing_h4_quiet_rerun quiet-rerun | 1 | `FAIL host_fenced_the_old_transport fenced=0` GATE FAIL `...\232519` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_232519` | `host_fenced_the_old_transport` only | (re-run) |
| heal | 0 | `{"pass": true, "failed": []}` | `D:\mx\fg6bat2\heal` | | yes |
| heal_wp | 1 | `{"pass": false, "failed": ["host_script_continued", "client_script_continued"]}` | `D:\mx\fg6bat2\heal_wp` | `host_script_continued`; `client_script_continued` | yes (known b) |
| fencing_1 | 0 | `GATE fencing_two_transports PASS -> ...\232625` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_232625` | | yes |
| fencing_2 | 0 | `GATE … PASS -> ...\232657` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_232657` | | yes |
| fencing_3 | 0 | `GATE … PASS -> ...\232729` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_232729` | | yes |
| fencing_4 | 0 | `GATE … PASS -> ...\232801` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_232801` | | yes |
| fencing_5 | 0 | `GATE … PASS -> ...\232834` | `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_232834` | | yes |
| reclaim (w62-2) | 0 | `GATE reclaim_socket PASS -> D:\mx\fg6bat2\reclaim\reclaim_socket_20260912_232906` | `D:\mx\fg6bat2\reclaim\reclaim_socket_20260912_232906` | | yes |
| compare_overlap | 0 | three `PASS sim-gated` lines | `D:\mx\fg6bat2\reclaim_overlap` | | yes |
| fixtures | 1 | `GAMEPLAY FIXTURES: FAIL (15 checks)` | `D:\mx\fg6bat2\gameplay` | `binary_matches_source` stale `NetReconnectSelfTest.cpp`; all `.semantic` PASS; no `.ToArray` this run | flipped red vs W81-4b (finding d) |
| crab_ai_order | 1 | `GAMEPLAY FIXTURES: FAIL (3 checks)` | `D:\mx\fg6bat2\gameplay_crab` | `binary_matches_source`; `_crab.ps1:84` Argument types; semantic PASS | flipped red vs W81-4b (finding d + known c) |
| dedicated_ded3 | 0 | `GATE dedicated_host PASS -> ...\233415` | `D:\mx\fg6bat2\dedicated\dedicated_host_20260912_233415` | | yes |
| dedicated_drop | 0 | `GATE dedicated_host PASS -> ...\233440` | `D:\mx\fg6bat2\dedicated\dedicated_host_20260912_233440` | | yes |
| P3COOP | 0 | `GATE lifecycle_P3COOP_a1 PASS -> ...\233544` | `D:\mx\fg6bat2\lifecycle\P3COOP_a1_20260912_233544` | | yes |
| P4PVPVE | 0 | `GATE lifecycle_P4PVPVE_a1 PASS`; `PASS p4_resumed_at_drop_frame` drop_frame=256 resumed_start all 257 | `D:\mx\fg6bat2\lifecycle\P4PVPVE_a1_20260912_233621` | | yes vs fg6 (exit 0); flipped green vs W81-4 `p4_resumed_at_drop_frame` |
| probe_right | engine=0 | `[net-directory-probe] PASS` | `D:\mx\fg6bat2\probe_right` | | yes |
| probe_wrong | engine=1 | `[net-directory-probe] POST /v1/sessions -> status=0 error=certificate pin mismatch body=` | `D:\mx\fg6bat2\probe_wrong` | expected mismatch | yes |
| probe_signal | engine=0 | `[net-directory-signal-probe] PASS` | `D:\mx\fg6bat2\probe_signal` | | yes |
| session_directory tests | 0 | `Ran 29 tests in 38.529s` / `OK` | `D:\mx\fg6bat2\session_directory_tests` | | flipped green (was 25 / 24) |
| directory_e2e | 0 | both arms exit 0; enabled url=`127.0.0.1:8457`; compared_ticks=600 | `D:\mx\fg6bat2\dir-e2e` | | yes |
| boot | 0 | settings_unchanged true; 174 lines; no install key | `D:\mx\fg6bat2\boot` | | yes |
| present_identity | 1 | sha_match_tip_tip2=true; compared_ticks=0 | `D:\mx\fg6bat2\present` | TICKS=180; traces 181 | yes (known artefact) |
| lobby_rejection | 0 | `{"pass": true, "failed": []}` | `D:\mx\fg6bat2\lobby_rejection` | | flipped green vs W81-4 |
| lobby_input_delay | 0 | `{"pass": true, "failed": []}` | `D:\mx\fg6bat2\lobby_input_delay` | | yes |
| fl100 | 0 | `fl100 PASS []` | `D:\mx\fg6bat2\fl` | | yes |
| mixed_lua | 0 | both peers exit 0; compared_ticks=600; hash `b3a5bd64…878e77` | `D:\mx\fg6bat2\mixed` | | yes |
| heal_global_d0 | 0 | `{"pass": true}` | `D:\mx\fg6bat2\heal_global_d0` | | yes |
| heal_global_d3 | 0 | `{"pass": true}` | `D:\mx\fg6bat2\heal_global_d3` | | yes |
| arm1 | 1 | `RESULT: FAIL [binary_matches_source]`; checker `PASS switch_control` | `D:\mx\fg6bat2\arm1` | `binary_matches_source` stale `NetReconnectSelfTest.cpp` | new (semantic switch_control PASS) |
| drop3_switch | 0 | `GATE peers_3_4_regression PASS -> ...\drop3\...\234414` | `D:\mx\fg6bat2\drop3\peers_3_4_regression_20260912_234414` | | new (N5; fg6 did not run) |
| preview_event_d7 | 0 | W104b `the_first_round_is_visible_on_the_preview_tick` PASS; `the_projectile_is_adopted_once` PASS | `D:\mx\fg6bat2\preview-event-d7` | | new (expected flip 5) |
| lpinv_100 | 0 | `[lpinv] PASS tick 100: 8/8 cases left the canonical world untouched` | `D:\mx\fg6bat2\lpinv-100` | | new (N5; fg6 did not run) |
| portmap_e2e | 0 | on/off both host/client exit 0; compared_ticks=600 | `D:\mx\fg6bat2\portmap` | | new (N4; fg6 did not run) |
| controller_log | 1 | missing result file; no `-controller-log` fixture | | none found | yes (removed from baseline greens) |
| ak47hud_1 | 0 | `[preview-hud-selftest] PASS press tick 500`; launch.exit=0; no dump | `D:\mx\fg6bat2\ak47hud\run1` | | new (N1) |
| ak47hud_2 | 0 | same HUD PASS; launch.exit=0; no dump | `D:\mx\fg6bat2\ak47hud\run2` | | new (N1) |
| ak47hud_3 | 0 | same HUD PASS; launch.exit=0; no dump | `D:\mx\fg6bat2\ak47hud\run3` | | new (N1) |
| ak47hud_compare | 1 | raw sha256 not identical; `compare_sim_traces.py` both pairs `PASS sim-gated: 600 overlapping ticks` | `ak47hud_compare.log` | byte hashes differ | new (N1) |
| expire3 | 0 | GATE PASS; checker `PASS claimed_actor_expiry` later=520 | `D:\mx\fg6bat2\expire3\peers_3_4_regression_20260912_234659` | | new (N2) |
| ice_e2e | 0 | `verdict.json` `"pass": true`; `traces_identical` ok | `D:\mx\fg6bat2\ice-e2e` | | new (N3) |

## FAIL checks verbatim

### net-reconnect (`D:\mx\fg6bat2\selftests\net-reconnect\stdout.log:1`)

Ran at battery start (before the HEAD move). Same line in suite `D:\mx\fg6bat2\suite\net-reconnect-selftest\stdout.log`.

```
[net-reconnect-selftest] FAIL: reclaim proof known-answer mismatch: 56527b38d2db7c0d08f6eb8a9ef61bf8d30ca76b31021009b20b1701cba13b9a
```

W81-4 had `[net-reconnect-selftest] PASS`. Not in the expected-flip list.

### suite (`D:\mx\fg6bat2\suite\result.json`)

`passed=10` `total=11`. Expected flip (6) was 11/11. The only red member is net-reconnect (same known-answer line). Other 10 members pass.

### fencing_two_transports / fencing_h4 (known a — old h4gates copy)

First battery run `D:\mx\fg6bat2\h4\fencing_two_transports_20260912_220413`:

```
  FAIL host_fenced_the_old_transport fenced_disconnects+fenced_packets=0
GATE fencing_two_transports FAIL
```

Quiet re-run `...\223255`: same `fenced=0` FAIL; other checks PASS.

remaining `fencing_h4_quiet_rerun.log` (`...\230200`):

```
  FAIL second_client_committed
  FAIL host_bound_second_incarnation 1
  FAIL host_fenced_the_old_transport fenced_disconnects+fenced_packets=0
  FAIL first_client_superseded
GATE fencing_two_transports FAIL -> D:\mx\fg6bat2\h4\fencing_two_transports_20260912_230200
```

Quiet re-run `fencing_h4_quiet_rerun_rerun.log` (`...\232519`):

```
  FAIL host_fenced_the_old_transport fenced_disconnects+fenced_packets=0
GATE fencing_two_transports FAIL -> D:\mx\fg6bat2\h4\fencing_two_transports_20260912_232519
```

fencing_1..5 through `fencing_h4gates` are the real gate (all GATE PASS).

### heal_wp (`heal_wp_stdout.txt` / `D:\mx\fg6bat2\heal_wp\result.json`) — known b

```
resync_heal PASS []
{"case": "heal_mod_semantics", "pass": false, "failed": ["host_script_continued", "client_script_continued"]}
```

`waypoints_match_across_peers` status=`pass` in `result.json`.

### fixtures (`fixtures.log`) — finding (d) + semantic PASS

```
[FAIL] binary_matches_source: stale binary: Source/Network/NetReconnectSelfTest.cpp (2026-09-12T22:49:32.8849874Z) is newer than the exe (2026-09-12T21:52:40.4024475Z); rebuild before testing
[FAIL] fire_reload.e2e: checks=17 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay\fire_reload
[FAIL] weapon_switch.e2e: checks=17 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay\weapon_switch
[FAIL] jetpack.e2e: checks=17 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay\jetpack
[FAIL] terrain_fire.e2e: checks=17 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay\terrain_fire
[FAIL] pie_reload.e2e: checks=17 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay\pie_reload
[FAIL] ai_orders.e2e: checks=18 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay\ai_orders
[FAIL] craft_cargo.e2e: checks=18 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay\craft_cargo
GAMEPLAY FIXTURES: FAIL (15 checks) evidence D:\mx\fg6bat2\gameplay
```

Every matching `.semantic` line is `[PASS]`. No `test_gameplay_fixtures.ps1:80` `.ToArray` this run.

### crab_ai_order (`crab_ai_order.log`) — finding (d) + known c

```
[FAIL] binary_matches_source: stale binary: Source/Network/NetReconnectSelfTest.cpp (2026-09-12T22:49:32.8849874Z) is newer than the exe (2026-09-12T21:52:40.4024475Z); rebuild before testing
[FAIL] crab_ai_order.e2e: checks=17 failed=[binary_matches_source] D:\mx\fg6bat2\gameplay_crab\crab_ai_order
[PASS] crab_ai_order.semantic: PASS crab_ai_order: seated actor 1048736 (Crab) from tick 35, peers identical over 27..400
OperationStopped: D:\Projects\reviews\takeover-20260909\grok-workers\fg6b-battery\test_gameplay_fixtures_crab.ps1:84
     | Argument types do not match
GAMEPLAY FIXTURES: FAIL (3 checks) evidence D:\mx\fg6bat2\gameplay_crab
```

### arm1 (`D:\mx\fg6bat2\arm1\driver.log` / `arm1.check.txt`)

```
RESULT: FAIL [binary_matches_source] D:\mx\fg6bat2\arm1
PASS switch_control: uid=1048634 apply=80 handback=90 owner_log=291 traces=600
```

`result.json` detail: same stale-binary sentence as fixtures. Sim-gated host==client PASS.

### present_identity (`D:\mx\fg6bat2\present\summary.json`) — known artefact

tip/tip2/ref sha256 all `418ef07f9ab874b2e3bcf8edbb8007582100be59aace30ab22c7d18b03aa85cc`; ticks=181; `compared_ticks`=0; reasons `host: has 181 ticks, expected exactly 180` / `client: has 181 ticks, expected exactly 180`. `sha_match_tip_tip2=true` `sha_match_tip_ref=true`.

### global-callback (`D:\mx\fg6bat2\selftests\global-callback\stdout.log` / `launch.json`)

`exit_code=1` `timed_out=false` `elapsed_seconds=2.007` `verdict_lines=[]`.

```
[global-callback-selftest] REFUSE needs -net-replay <recording> or -scenario, and UserScenes.rte Checkpoint Global
```

gcb extra step (`tools/test_global_callbacks.py`) `pass=true`.

### controller_log

Missing result file. No `-controller-log` fixture under `stage2_p4` or `control-build\tools`.

### ak47hud_compare (N1)

Raw file hashes not equal (see N1). `compare_sim_traces.py` both pairs exit 0 with `PASS sim-gated: 600 overlapping ticks identical (controller excluded); paused_ticks=0`.

## N1 — W130 / W90 AK-47 HUD replay

Exact command (N=1,2,3):

```
python D:\Projects\control-build\tools\run_ak47_replay.py --repo D:\Projects\control-build --out D:\mx\fg6bat2\ak47hud\run<N> --timeout 900 --replay D:\Projects\stage2_p4\fixtures\ak47_fire.ccreplay --script D:\Projects\stage2_p4\fixtures\ak47_fire.txt --max-ticks 600 -- -local-prediction-depth 7 -local-prediction-hud 500
```

| run | launch.exit | timed_out | dump | HUD line (`stdout.log:22`) |
|---|---|---|---|---|
| run1 | 0 | false | none | `[preview-hud-selftest] PASS press tick 500` |
| run2 | 0 | false | none | `[preview-hud-selftest] PASS press tick 500` |
| run3 | 0 | false | none | `[preview-hud-selftest] PASS press tick 500` |

Quoted HUD block (identical wording on all three; run1 `stdout.log:17-22`):

```
[preview-hud-selftest] PASS the_ammo_readout_follows_the_preview: drawn rounds 30 outcome rounds 30
[preview-hud-selftest] PASS the_carousel_follows_the_preview: draw actor is the preview, center delta 0.000000, equipped 'AK-47' vs 'AK-47'
[preview-hud-selftest] PASS the_activity_cursor_survives_a_preview: IsActor=1 ValidMO=1 on the substituted actor
[preview-hud-selftest] PASS a_preview_timer_reset_reads_forward: min clone timer elapsed 0.000000 ms
[preview-hud-selftest] PASS no_canonical_hud_write: canonical pie enabled 1->1 visible 1->1
[preview-hud-selftest] PASS press tick 500
```

Playback: `outcome=tick_cap frames=597 last_tick=600 end_marker=0 exit=0`. No crash line, no FATAL, no EXCEPTION, no `.dmp`.

Compare command:

```
python D:\Projects\control-build\tools\compare_sim_traces.py D:\mx\fg6bat2\ak47hud\run1\trace.json D:\mx\fg6bat2\ak47hud\run2\trace.json
python D:\Projects\control-build\tools\compare_sim_traces.py D:\mx\fg6bat2\ak47hud\run1\trace.json D:\mx\fg6bat2\ak47hud\run3\trace.json
```

Raw sha256 (not byte-identical):

- run1 `51f94572c976504c6dfecaa10780ecf778eaa4b00550f91d485f3bda605cffbd`
- run2 `90b4c3921073510e023ea59d5bd40dd7825fe6b41f16ad41eaeba0f687c470e8`
- run3 `aa74e928742ddc9985a7dc8f6ce20d010a7ecc7a05e89d86c2fee09a45b5584c`

Known open defect F15 (preview-HUD crash) did not write a dump on these three runs.

## N2 — W119 expire3

Exact command:

```
python D:\Projects\reviews\takeover-20260909\grok-workers\fg6b-battery\w119_h4gates\peers_3_4_regression.py --arm expire3 --port-expire 47651
```

`CC_H4_RUN_ROOT=D:/mx/fg6bat2/expire3`. Root `D:\mx\fg6bat2\expire3\peers_3_4_regression_20260912_234659`.

`expire3.log`:

```
  PASS expire3_survivors_exit_zero leave_frame=82 survivors=['expire3_host', 'expire3_client2'] exits={'expire3_host': 0, 'expire3_client2': 0}
  PASS expire3_survivors_identical 2 traces identical
  PASS expire3_owner_log_returned_to_host_ai last_claim_tick=81 expiry_tick=82 later=520 last_claim_tick=81 expiry_tick=82 later=520 host_ok=True client_ok=True identical=True sample=[{'tick': 82, 'uid': 1048634, 'owner': 1, 'mode': 2}, ...]
  PASS expire3_returned_line {'expire3_host': '[net-match] claim of actor 1048634 returned to peer 1 after seat 2 expired', 'expire3_client2': '[net-match] claim of actor 1048634 returned to peer 1 after seat 2 expired'}
GATE peers_3_4_regression PASS
```

Checker:

```
python D:\Projects\control-build\tools\check_claimed_actor_expiry.py <host_report> <client2_report> <host/stdout.log> <client2/stdout.log> <host_trace> <client2_trace>
```

`expire3_checker.log`:

```
PASS claimed_actor_expiry: later=520 returned=[net-match] claim of actor 1048634 returned to peer 1 after seat 2 expired
```

Checker exit 0.

## N3 — W123 loopback session-id join e2e

Exact command:

```
python D:\Projects\control-build\tools\test_directory_ice_join.py --out D:\mx\fg6bat2\ice-e2e --port 8457 --game-port 47656 --ticks 600 --cert D:\mx\w75\cert.pem --key D:\mx\w75\key.pem
```

Cert/key existed. No non-loopback address. `ice_e2e.log`:

```
[ice-e2e] OK   traces_identical ['PASS sim-gated: 601 overlapping ticks identical (controller excluded); paused_ticks=0']
GATE ice-session-join PASS D:\mx\fg6bat2\ice-e2e\verdict.json
```

`D:\mx\fg6bat2\ice-e2e\verdict.json`: `"pass": true`; check `traces_identical` ok=true with the same `PASS sim-gated: 601 overlapping ticks` detail; `directory_url`=`127.0.0.1:8457`; host/client exit 0.

## Heal results

(a) `python heal_run.py D:\mx\fg6bat2\heal` exit 0. `heal_stdout.txt`:

```
resync_heal PASS []
{"case": "heal_mod_semantics", "pass": true, "failed": []}
```

(b) `python heal_wp_run.py D:\mx\fg6bat2\heal_wp --ai-write-script` exit 1. script_continued failed (quoted above). waypoints_match_across_peers pass.

## Five fencing runs (fencing_h4gates)

`CC_H4_RUN_ROOT=D:/mx/fg6bat2/h4`. Every check line in `fencing_1.log` … `fencing_5.log` is PASS. `is not owed frame` per run:

- fencing_1 YES (`...\232625\host\stdout.log:9` `Client's fenced connection is not owed frame 251`); `waiver_logged=True` `peer_frames_waived=1`
- fencing_2 NO; `waiver_logged=False` `peer_frames_waived=0`
- fencing_3 YES (`...\232729\host\stdout.log:9` `… frame 260`)
- fencing_4 YES (`...\232801\host\stdout.log:9` `… frame 259`)
- fencing_5 NO; `waiver_logged=False` `peer_frames_waived=0`

## Reclaim (w62-2) + compare_overlap

`reclaim.log` every check PASS. GATE PASS `D:\mx\fg6bat2\reclaim\reclaim_socket_20260912_232906`.

`compare_overlap.log`:

```
PASS sim-gated: 901 overlapping ticks identical (controller excluded); paused_ticks=0
PASS sim-gated: 657 overlapping ticks identical (controller excluded); paused_ticks=0
PASS sim-gated: 657 overlapping ticks identical (controller excluded); paused_ticks=0
```

## Gameplay fixtures

Semantic `[PASS]` lines as in `fixtures.log` / `crab_ai_order.log`. Wrapper `.e2e` lines FAIL on `binary_matches_source` because `NetReconnectSelfTest.cpp` became newer than the pinned exe during the remaining steps (external HEAD `5c2c65c2ed`). W81-4b had `binary_matches_source` PASS on a newer-than-source exe. Brief (d): a stale-binary red on this exe is a finding.

## Dedicated host

ded3 (`dedicated_ded3.log`): all PASS including `ded3_sim_gated_identical 3 traces identical`; GATE `...\233415`.

drop: every check PASS. GATE `...\233440`.

## Lifecycle packs

P3COOP all PASS. GATE `...\233544`.

P4PVPVE GATE PASS. `p4_resumed_at_drop_frame` PASS (`drop_frame=256` `resumed_start` host/client2/client3/returner all 257). Rematch checks PASS. W81-4 had this check FAIL.

## Directory probe (port 8457)

Right pin `5d522abdb598edcdb71192249c48509dcea714d48a36249dc2d001162663d501`, engine exit 0: `[net-directory-probe] PASS`.

Wrong pin `8d522abd…`, engine exit 1:

```
[net-directory-probe] POST /v1/sessions -> status=0 error=certificate pin mismatch body=
```

Signal probe, engine exit 0: `[net-directory-signal-probe] PASS`.

Service tests: `Ran 29 tests in 38.529s` / `OK` (`session_directory_tests.log:1278-1280`). Expected flip (1).

## Directory e2e

`lead_w75_e2e.py --arms enabled,disabled` exit 0. `D:\mx\fg6bat2\dir-e2e\lead_verdict.json`: enabled url=`127.0.0.1:8457` host/client exit 0; host directory registers=1 heartbeats=11 deletes=1 state=`deleting`; client state=`disabled`; compared_ticks=600 first_divergence=null. Disabled arm both exit 0, compared_ticks=600.

## Boot settings-unchanged

`settings-gate.json`: staged_settings_sha256=`c1a19b747968afe072d6c1d5c6488a6d440e8b96a0dc9449256ddaa56abd3894` equals after_settings_sha256_text; after_settings_lines=174; after_install_key_lines=`[]`; settings_unchanged=true; exit_code=0.

## Presentation identity + lobby drivers

`D:\mx\fg6bat2\present` TICKS=180: compared_ticks=0 (quoted above).

`tools/test_lobby_rejection.py --port 47654` exit 0: `{"pass": true, "error": null, "failed": []}`.

`tools/test_lobby_input_delay.py --port 47655` exit 0: `{"pass": true, "error": null, "failed": []}`.

## fl100

`fl100.log`: `fl100 PASS []`.

## Mixed-count duel (W85)

`w85_e2e.py --arms disabled --host-lua-states 32 --client-lua-states 4` port 47651. Both exit 0. running_ticks host=601 client=601. trace_compare compared_ticks=600 first_divergence=null. Final hashes both `b3a5bd641b88be88310bb1d075c3885ba6a96ea93c73990b71de352995878e77`. Refusal lines empty.

## Heal global (Source43)

d0 `D:\mx\fg6bat2\heal_global_d0`: `"pass": true`. d3 `D:\mx\fg6bat2\heal_global_d3`: `"pass": true`.

## N4 portmap_e2e / N5 drop3_switch, lpinv_100, controller_log

portmap_e2e: copied `w97_port_map_e2e.py` (loopback fakes; no `-net-port-map-probe`). Driver used service `127.0.0.1:8469` and game port 47603 (its own defaults). on/off both peers exit 0; compared_ticks=600; on mapped `udp 47603 -> 203.0.113.7:47603 via natpmp`.

drop3_switch: `w102_h4gates\peers_3_4_regression.py --arm drop3 --port-drop 47653` GATE PASS; drop3_distinct_seats seats host=1 client2=3 returner=2.

lpinv_100: `[lpinv] PASS tick 100: 8/8 cases left the canonical world untouched`.

controller_log: missing (above).

## preview_event_d7 (expected flip 5)

`D:\mx\fg6bat2\preview-event-d7\stdout.log:17-18`:

```
[preview-event-selftest] PASS the_first_round_is_visible_on_the_preview_tick: first projectile for the press at committed tick 154 (event tick 161, seq 0, predicted=1), expected <= 154
[preview-event-selftest] PASS the_projectile_is_adopted_once: 1 adoptions of that projectile
```

`preview_ghosts_peak=2` on the localpred line.

## Resync-frame table

Full scan: `trace_scan.json`. `service.session_events.discarded` is 0 on every report that has the field. P4PVPVE traces contain the rematch junction (round-1 then round-2). Reclaim host/returner traces contiguous (host 1..901; returner 245..901). expire3 / drop3 / ice-e2e traces used by those gates are listed in their verdicts.

## Selftest lines (path:line)

- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:24` `[net-lockstep-selftest] PASS resync_applies_only_at_completed_tick frame=2 pumps=41`
- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:106` `[net-lockstep-selftest] PASS parked_wait_applies_pending_resync frame=1 pumps=1 ms=0`
- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:109` `[net-lockstep-selftest] PASS fenced_peer_waived_at_the_parked_tick frame=1 pumps=1`
- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:225` `[net-resync-runtime-selftest] PASS: deferred stop capture`
- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:250` `[net-lockstep-selftest] PASS claimed_actor_returns_to_cpu_after_the_claimant_expires`
- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:1` `[net-lockstep-selftest] PASS ai waypoint adds cross the wire: sent=3 owner_wp=2 peer_wp=2`
- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:162` `[net-lockstep-selftest] PASS value_observation_codec n=3`
- `D:\mx\fg6bat2\selftests\net-lockstep\stdout.log:251` `[net-lockstep-selftest] PASS`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:80` `[net-match-selftest] PASS healed round planned end: host_total=599 client_total=599 behind_total=599 at frame 599, both stop at 601`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:82` `PASS pending_session_event_survives_teardown peer_state=Closed drained=1 discarded=0 peer_frames_waived=1`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:84` `[net-match-selftest] mux listen order: the ICE half opens first (identity bound there), then the IP listen`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:85` `[net-match-selftest] mux routing: Send/Disconnect/ping follow the peer-id tag, ICE events come back tagged, an unbound fault stays invalid, posted tasks run inside PollEvents`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:86` `[net-match-selftest] ice join_mode: either with a direct address, ice without one, and ip once a rematch re-registers under a session id the pinned GNS identity no longer matches`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:87` `[net-match-selftest] session-id join: an absent, full, mismatched or ip-only row is refused with the join list's own label; an ice row resolves to str:h-<session>, an either row keeps its address`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:88` `[net-match-selftest] ice settings: NetworkIceEnable defaults off; -net-ice and -net-stun decide the run and never touch the saved value`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:89` `[net-match-selftest] p2p join spec: StartClient dials the config's spec and replays it on the SessionFull retry; without a spec the direct-IP Connect is unchanged`
- `D:\mx\fg6bat2\selftests\net-match\stdout.log:90` `[net-match-selftest] PASS`
- `D:\mx\fg6bat2\selftests\net-directory\stdout.log:25` `[net-directory-selftest] ice rows: join_mode ice/either is joinable without an address, ip without one still refuses "address", and a full ice row still refuses "full"`
- `D:\mx\fg6bat2\selftests\net-directory\stdout.log:26` `[net-directory] list: page failed, keeping 100 rows` (W120 case: failed later page keeps earlier rows)
- `D:\mx\fg6bat2\selftests\net-directory\stdout.log:74` `[net-directory-selftest] PASS install key lazy: load left 57 bytes unchanged, first use wrote key cc6624e050ab01468633b2d7b7267c40`
- `D:\mx\fg6bat2\selftests\net-directory\stdout.log:72` `[net-directory-selftest] http client cancel url=https://127.0.0.1:52243/ in_flight=true cancel_ms=0`
- `D:\mx\fg6bat2\selftests\net-directory\stdout.log:75` `[net-directory-selftest] PASS`
- `D:\mx\fg6bat2\selftests\controller-frame\stdout.log:1` `[controller-frame-selftest] PASS`
- `D:\mx\fg6bat2\selftests\net-reconnect-session\stdout.log:31` `[net-reconnect-session-selftest] PASS`
- `D:\mx\fg6bat2\selftests\net-protocol\stdout.log:1` `[net-protocol-selftest] PASS module digests: entry cap 256, byte cap 24576, full-cap list 14598 bytes`
- `D:\mx\fg6bat2\selftests\net-protocol\stdout.log:2` `[net-protocol-selftest] PASS chat: 128-byte cap, UTF-8 validated, scopes all/team`
- `D:\mx\fg6bat2\selftests\net-protocol\stdout.log:3` `[net-protocol-selftest] PASS old wire: v2 build stamps a JoinRejected at v1, refuses v2-only types there`
- `D:\mx\fg6bat2\selftests\net-identity\stdout.log:1` `[net-identity-selftest] PASS diff_modules by file name: This host's mods do not match yours. Install: Coalition.rte · Remove: MyMod.rte · Update: Ronin.rte (you 3, host 5)`
- `D:\mx\fg6bat2\selftests\net-identity\stdout.log:2` `[net-identity-selftest] PASS lua state count out of identity: 4 and 32 states share deterministic_config_hash 3e90c1986bc3ec9fdaf159a397be8fb0132c300e1ca6c82073c6e794a1562ceb and session_identity_hash 6f5d638aefd97b2bf6aeced9b551869c88ee1c8d0c45aae90df239c4b74a2b4d`
- `D:\mx\fg6bat2\selftests\net-session\stdout.log:2` `[net-session-selftest] PASS module mismatch names the modules: This host's mods do not match yours. Install: Coalition.rte · Remove: MyTestMod.rte · Update: Ronin.rte (you 3, host 5)`
- `D:\mx\fg6bat2\selftests\net-session\stdout.log:5` `[net-session-selftest] PASS v1 peer gets a v1-stamped rejection: protocol version 1 does not match this build's 2`
- `D:\mx\fg6bat2\selftests\native-graph\stdout.log:124` `[net-local-ui-selftest] PASS alias_from_frame_function_upvalue`
- `D:\mx\fg6bat2\selftests\native-graph\stdout.log:154` `[script-graph-selftest] PASS entity_cast_functions_cover_bound_classes`
- `D:\mx\fg6bat2\selftests\native-graph\stdout.log:155` `[script-graph-selftest] PASS background_owner_reference_restores`
- `D:\mx\fg6bat2\selftests\native-graph\stdout.log:733` `[script-graph-selftest] PASS refused_reinstate_leaves_the_next_hold_possible`
- `D:\mx\fg6bat2\selftests\net-p2p-gather\stdout.log:42` `[net-p2p-selftest] t=1634.9ms local candidates GNS queued: 5 (IPv4 5, IPv6 0) with P2P_Transport_ICE_Enable=2 (Private)`
- `D:\mx\fg6bat2\selftests\net-p2p-gather\stdout.log:43` `[net-p2p-selftest] PASS`
- `[global-callback-selftest] PASS` not present (REFUSE)

## Wall-clock stalls (`stalls.json`)

| utc | kind | waited_s | detail |
|---|---|---|---|
| 2026-09-12T22:24:17 | process | 1175.7 | Cortex Command |
| 2026-09-12T22:32:55 | process | 512.7 | Cortex Command |
| 2026-09-12T22:32:55 | quiet_unmet | ~0 | 20min without a 60s idle gap |
| 2026-09-12T22:36:33 | process | 181.0 | Cortex Command |
| 2026-09-12T22:45:11 | process | 452.7 | Cortex Command |
| 2026-09-12T22:57:23 | process | 60.5 | Cortex Command |
| 2026-09-12T22:58:04 | process | 30.3 | Cortex Command |
| 2026-09-12T22:59:26 | process | 30.3 | Cortex Command |
| 2026-09-12T23:00:58 | process | 30.3 | Cortex Command |
| 2026-09-12T23:02:00 | quiet | 61.8 | 60s no Cortex Command* |
| 2026-09-12T23:06:08 | process | 90.6 | Cortex Command |
| 2026-09-12T23:07:05 | process | 30.3 | Cortex Command |
| 2026-09-12T23:12:12 | process | 301.7 | Cortex Command |
| 2026-09-12T23:16:54 | process | 271.6 | Cortex Command |
| 2026-09-12T23:18:35 | process | 90.6 | Cortex Command |
| 2026-09-12T23:20:21 | process | 90.7 | Cortex Command |
| 2026-09-12T23:23:08 | process | 150.8 | Cortex Command |
| 2026-09-12T23:24:13 | process | 60.4 | Cortex Command |
| 2026-09-12T23:25:19 | process | 60.4 | Cortex Command |
| 2026-09-12T23:25:19 | quiet_unmet | ~0 | 20min without a 60s idle gap |
| 2026-09-12T23:35:44 | process | 30.2 | Cortex Command |
| 2026-09-12T23:39:19 | process | 30.3 | Cortex Command |

No `LEAD_EXCLUSIVE.lock` stall. Two `quiet_unmet` rows: the 20-minute idle-gap wait expired; the next launch still waited for process-clear.

Scratch `D:\mx\fg6bat2` 0.828 GB (under 8 GB). No directory tree under `D:\mx` was moved, copied, or deleted.
