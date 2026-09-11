# W21: design B (five item-6 secondary defects)

Worktree `D:\Projects\h4-secondary`, branch `stage2/h4-secondary`, cut from `be0a2672a9`. HEAD `0520bbc4f72859769fe9c1afcb8d51e420e48a63`. No push. Vendor `*.lib` files were not staged. Engine launches used `python D:\Projects\h4-secondary\tools\win32_test_runner.py` only.

Detecting tests were compiled against unfixed production first, then the production edits landed, then the same binaries were rebuilt and re-run.

## Item 1 — refused reclaim is a new join

Commit: `1c2b6c569a` (`NetReconnectSession.h`, `NetReconnectSessionSelfTest.cpp`). Report field `client_reclaim_outcome` is in `e6cb063014` (`NetMatchService.cpp:1075`).

Lines: `UsedStoredTicket()` is `m_UsedStoredTicket && !m_FellBackToNewJoin` (`NetReconnectSession.h:573`). `ReclaimOutcome()` returns `reclaim_accepted` / `new_join_after_refusal` (`:574-582`). Test `TestRefusedReclaimReportsNewJoin` in `NetReconnectSessionSelfTest.cpp`.

Before: `python ...\run_selftest.py item1-before 120 -headless -net-reconnect-session-selftest` → `D:\mx\w21\item1-before\stdout.log` last verdict `FAIL: used stored ticket stayed true after the fallback new join`. Copy: `evidence\item1-before.stdout.log`.

After: same command, out `D:\mx\w21\item1-after` → `[net-reconnect-session-selftest] PASS`. Copy: `evidence\item1-after.stdout.log`.

Gate driver (not committed): pre-copy `w21-h4-secondary\clean_leave.py.pre`. Live edit `reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\h4gates\clean_leave.py` check `clean_old_ticket_refused` now reads `client_reclaim_outcome != "reclaim_accepted"`. Diff: `w21-h4-secondary\clean_leave.diff`. The live file also has an extra comment `# Announced leave stays tick-based; no hold pause.` that this worker did not write.

## Item 2 — failed report keeps admission counters

Commit: `e6cb063014` (capture/synthesize) and `9d67a88b0d` (`TestFailedReportKeepsAdmissionCounters`).

Lines: `ReportRuntimeError` writes `m_CapturedRunnerReport` before teardown (`NetMatchService.cpp:638-639`). `BuildReportJson` emits live runner, else captured JSON, else synthesized `runner.session.admission` plus `session.stats.fenced_*` (`:1173-1185`, helper `:58-104`). Fields named from `tools/h4_substitution_gates.py` and `h4gates/*.py`: `seats_dropped`, `applicants_registered`, `substitutions_committed`, `reclaims_accepted`, `incarnations_bound`, `substitutions_cancelled`, `applicants_refused`, `pending_applicants`, `substitution_offers_sent`, `substitution_offer_retransmits`, `replayed_results`, `seats_closed_by_leave`, `fenced_packets`, `fenced_disconnects`, `new_joins`.

Before: `run_selftest.py item2-3-5-before 120 -headless -net-match-selftest` → `D:\mx\w21\item2-3-5-before\stdout.log`: `FAIL: failed report omitted runner.session.admission`.

After: `D:\mx\w21\item2-3-5-after\stdout.log`: `[net-match-selftest] PASS`.

## Item 3 — rejoin while activity Over

Commits: `e6cb063014` (`ResyncSnapshotAllowed` / `ClassifyRejoin` / `AnswerMatchOverRejoin` / `PumpSessionEvents` / `ResyncMatch`), `72fd1352c0` (`Main.cpp` Complete:/`match over` paths), `9d67a88b0d` (`TestRejoinWhileActivityOver`).

Lines: `NetMatchService.cpp:241-271`, `:288-293`, `:970-990`. Existing `NetDisconnect{SessionEnded}` to a session-Ready joiner; no new wire type. `FinishMatch` keeps the session for survivors. `Main.cpp` treats `resyncError == "match over"` and `Complete:` + match-over as a clean end (no `ReportRuntimeError`).

Before: same match run as item 2 → `FAIL: an over or missing activity still asked for a resync snapshot`.

After: `[net-match-selftest] PASS`.

## Item 4 — sender-side team authority

Commit: `0520bbc4f7`.

Lines: `ScenarioRunner.cpp:1078-1083` (skip `Reseat`; else require `IsLockstepTeamCommandSender`). `Actor.cpp:1477-1479` (PopWaypoint also requires the team sender check). Receiver in `MovableMan.cpp` unchanged. Test `TestSenderDropsUncontrolledTeamCommands` in `NetLockstepSelfTest.cpp`.

Before: `run_selftest.py item4-before 180 -headless -net-lockstep-selftest` → `D:\mx\w21\item4-before\stdout.log`: `FAIL: host enqueued an AIOrder for a team it does not control`.

After: `D:\mx\w21\item4-after\stdout.log`: `[net-lockstep-selftest] PASS`.

## Item 5 — e2e running ticks across resync

Commits: `e6cb063014` (`NetMatchE2ETickClock` in `NetMatchService.h`; `OnResyncRelaunch` adds `segmentTicks` into `priorTicks`), `72fd1352c0` (`Main.cpp` uses `Total()` / `EarlyOverIsSetupFailure()` / `OnResyncRelaunch` / `OnNewMatch`; rematch still zeros), `9d67a88b0d` (`TestE2ETickClockSurvivesResync`).

Before: same match run as item 2 → `FAIL: e2e running ticks reset at resync; total=0`.

After: `[net-match-selftest] PASS`.

## Selftest table

Command: `python D:\Projects\reviews\takeover-20260909\grok-workers\w21-h4-secondary\run_battery.py` → `w21-h4-secondary\battery.json`. Exe sha256 `26fb4256a49a56223b75d735aa997d5f182ea516756ec6e901a233dc3cf61b2b`.

| Case | Exit | Last verdict | Out |
|---|---|---|---|
| protocol | 0 | `[net-protocol-selftest] PASS` | `D:\mx\w21\battery\protocol` |
| identity | 0 | `[net-identity-selftest] PASS` | `D:\mx\w21\battery\identity` |
| session | 0 | `[net-session-selftest] PASS` | `D:\mx\w21\battery\session` |
| lockstep | 0 | `[net-lockstep-selftest] PASS` | `D:\mx\w21\battery\lockstep` |
| match | 0 | `[net-match-selftest] PASS` | `D:\mx\w21\battery\match` |
| auth | 0 | `[net-auth-selftest] PASS` | `D:\mx\w21\battery\auth` |
| admission | 0 | `[net-admission-selftest] PASS` | `D:\mx\w21\battery\admission` |
| reconnect | 0 | `[net-reconnect-selftest] PASS` | `D:\mx\w21\battery\reconnect` |
| reconnect-session | 0 | `[net-reconnect-session-selftest] PASS` | `D:\mx\w21\battery\reconnect-session` |
| discovery | 0 | `[net-discovery-selftest] PASS` | `D:\mx\w21\battery\discovery` |
| controller-frame | 0 | `[controller-frame-selftest] PASS` | `D:\mx\w21\battery\controller-frame` |
| script-graph | 1 | see fails | `D:\mx\w21\battery\script-graph` |

`script-graph` stdout (`D:\mx\w21\battery\script-graph\stdout.log`, copy `evidence\script-graph.stdout.log`):

- `:138` `[net-local-ui-selftest] FAIL unreferenced_owner_reclaimed`
- `:384` `[script-graph-selftest] FAIL native_runtime_checkpoint_values`
- `:684` `[script-graph-selftest] FAIL`

Those two names also appear in `reviews\takeover-20260909\grok-workers\w12-windows-compile\`. No production change in this branch touches local-UI retention or script checkpoint values. Ten network CLI + controller-frame exited 0.

## Other

Build: `CL=/MP6`, `RTEA.sln` `/t:RTEA` Final x64, env `GNS_ROOT` / `GNS_DEP_ROOT` from RESUME §D. Runtime DLLs were copied beside the exe from `p4b-interp-validation` (not committed). `Userdata\` created for the runner (not committed). No new protocol types. Uncommitted remainder: the two vendor `*.lib` files only.
