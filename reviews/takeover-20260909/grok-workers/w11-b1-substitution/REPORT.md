# W11 — causal trace of the two red B1 substitution gates

Read-only on the approved tree and `D:\mx\s41b3`. Writes only under this directory. No engine launch, no build.

Commands:

```
python -B D:\Projects\reviews\takeover-20260909\grok-workers\w11-b1-substitution\extract_evidence.py
```

Outputs: `evidence/report_fields.json`, `evidence/resume_hits.txt`.

Pin (from `D:\mx\s41b3\j43\...\verdict.json` and host `launch.json`): exe `bb3cf264b4e7304f45486440998e4b63e37d5d46d0d119caa04f76c2fc378ffb`. Family Source41 breadth v3.

| Case | Run root | Gate result |
|---|---|---|
| `b1_substitute_commit` | `D:\mx\s41b3\j43\substitute_commit_20260910_022955` | FAIL |
| `b1_substitute_returner_wins` | `D:\mx\s41b3\j44\substitute_returner_wins_20260910_023039` | FAIL |
| `b1_substitute_host_cancel` | `D:\mx\s41b3\j45\substitute_host_cancel_20260910_023124` | PASS (control) |
| `b1_substitute_bounds` | `D:\mx\s41b3\j46\substitute_bounds_20260910_023246` | PASS (control) |

---

## Reader vs writer (the `None` fields)

Gate reader (`tools/h4_substitution_gates.py`):

```100:105:D:\Projects\p4b-interp-validation\tools\h4_substitution_gates.py
def admission_of(report) -> dict:
    service = service_of(report)
    runner = service.get("runner")
    session = runner.get("session") if isinstance(runner, dict) else None
    admission = session.get("admission") if isinstance(session, dict) else None
    return admission if isinstance(admission, dict) else {}
```

```326:345:D:\Projects\p4b-interp-validation\tools\h4_substitution_gates.py
        "seat_dropped",
        (admission.get("seats_dropped") or 0) >= 1,
        admission.get("seats_dropped"),
    )
    ...
            "applicant_registered",
            (admission.get("applicants_registered") or 0) >= 1,
            admission.get("applicants_registered"),
        )
        ...
            "substitution_committed",
            admission.get("substitutions_committed") == 1,
            admission.get("substitutions_committed"),
```

```373:395:D:\Projects\p4b-interp-validation\tools\h4_substitution_gates.py
            "returner_reclaimed",
            (admission.get("reclaims_accepted") or 0) >= 1,
            admission.get("reclaims_accepted"),
        )
        ...
            "seat_has_one_holder",
            (admission.get("incarnations_bound") or 0) >= 1,
            admission.get("incarnations_bound"),
```

Session writer (only while `m_ReconnectHost` is attached to a live `NetSession`):

```1078:1128:D:\Projects\p4b-interp-validation\Source\Network\NetSession.cpp
		json admission = json::object();
		if (m_ReconnectHost) {
			const NetReconnectHostStats& stats = m_ReconnectHost->GetStats();
			admission = json{
                ...
				{"reclaims_accepted", stats.reclaimsAccepted},
                ...
				{"incarnations_bound", stats.incarnationsBound},
				{"seats_dropped", stats.seatsDropped},
                ...
				{"applicants_registered", stats.applicantsRegistered},
                ...
				{"substitutions_committed", stats.substitutionsCommitted},
```

Service writer nests that under `runner` only when runner+session+coordinator are still held, and always writes a smaller reconnect subset:

```878:930:D:\Projects\p4b-interp-validation\Source\Network\NetMatchService.cpp
			{"host_seats_dropped", m_ReconnectHost.GetStats().seatsDropped},
			{"host_ledger_drops_recorded", m_ReconnectHost.GetStats().ledgerDropsRecorded},
			{"host_reseats_issued", m_ReconnectHost.GetStats().reseatsIssued},
			{"host_reseats_without_a_ledger", m_ReconnectHost.GetStats().reseatsWithoutALedger},
			{"host_reseats_without_survivors", m_ReconnectHost.GetStats().reseatsWithoutSurvivors},
			{"host_reseat_live_on_team_not_named", m_ReconnectHost.GetStats().reseatLiveOnTeamNotNamed},
		};
        ...
		if (m_Runner && m_Session && m_Coordinator) {
			report["runner"] = json::parse(m_Runner->BuildReportJson(*m_Session, *m_Coordinator));
		}
```

E2E report wrapper (`Main.cpp:3450-3481`) always emits `service` via `g_NetMatchService.BuildReportJson()`. It does not write `seats_dropped` / `applicants_registered` / `substitutions_committed` / `reclaims_accepted` / `incarnations_bound` at the top level.

`Complete("e2e complete")` (`NetMatchService.cpp:526-537`) leaves runner/session alive. `ReportRuntimeError` does not:

```490:518:D:\Projects\p4b-interp-validation\Source\Network\NetMatchService.cpp
	void NetMatchService::ReportRuntimeError(const std::string& error) {
        ...
			runner = std::move(m_Runner);
			coordinator = std::move(m_Coordinator);
			session = std::move(m_Session);
            ...
			m_IsHost = false;
			m_LocalPeerId = 0;
            ...
			m_State = NetMatchServiceState::Failed;
			m_StatusText = "Match stopped";
			m_ErrorText = error;
            ...
			EndAdmissionSession();
```

`EndAdmissionSession` (`NetMatchService.cpp:377-387`) sets `m_AdmissionAttached = false` and clears `m_SeatStatuses`. `EndHostedSession` (`NetReconnectSession.cpp:1096-1126`) clears ledger/applicants/substitutions/seats; it does not zero `m_Stats`, which is why `reconnect.host_seats_dropped` can still read `1` after teardown.

---

## Passing B1 controls (same family, same exe, same `-net-match-e2e-resync`)

Both pass because nobody becomes session-Ready without a lockstep remote, so `RequestResync` never runs. Host/stayer exit 0 via `Complete("e2e complete")`. Host report keeps `service.runner.session.admission`.

`substitute_host_cancel` (`j45`, `verdict.json` `passed=true`):

- Asserts: `host_exit_zero 0`, `stayer_exit_zero 0`, `seat_dropped 1`, `host_approved`, `host_cancelled`, `substitution_cancelled 1`, `substitution_did_not_commit 0`, `substitute_not_joined Substituting`, `seat_stayed_reassignable`.
- Host log (`host\stdout.log:8-11`): drop at frame 991; `moderation: substitute seat 1 -> Ok`; `moderation: cancel seat 1 -> Ok`; no `rejoin` / `resync`.
- Host report (`evidence/report_fields.json` cancel): `resyncs=0`, `running_ticks=3601`, `exit_code=0`, `is_host=true`, `has_runner=true`, `admission.seats_dropped=1`, `applicants_registered=1`, `substitutions_cancelled=1`, `substitutions_committed=0`, `incarnations_bound=2`, `reclaims_accepted=0`, `seat_holds_expired=1`. `reconnect.host_seats_dropped=1` matches admission. `reconnect.moderation` lists seat 1 `dropped=true`, `substituting=false`.

`substitute_bounds` (`j46`, `passed=true`):

- Asserts: `seat_dropped 1`, `two_applicants_registered 2`, `third_applicant_refused 5`, `bound_respected 0`, `nothing_committed 0`, applicants inert.
- Host log (`host\stdout.log:8-10`): drop at frame 992; no substitute approval; no resync.
- Host report: `resyncs=0`, `running_ticks=3601`, `exit_code=0`, `admission.seats_dropped=1`, `applicants_registered=2`, `applicants_refused=5`, `substitutions_committed=0`, `incarnations_bound=2`.

Both controls also have `activity_state=Over` and `winner_team=2` at the tick cap. Over after `running_ticks>=100` is not an e2e failure (`Main.cpp:2933-2940`).

---

## 1. `substitute_commit`

### Failing checks (quoted)

`D:\mx\s41b3\j43\substitute_commit_20260910_022955\verdict.json:18-60`:

- `host_exit_zero` fail `1`
- `stayer_exit_zero` fail `1`
- `seat_dropped` fail `None`
- `applicant_registered` fail `None`
- `substitution_committed` fail `None`

Passing on the same verdict: `host_adjudicated_the_drop`, `host_census_clean 0`, `host_approved`, `substitute_offered_a_ticket`, `substitute_joined Joined`, `substitute_stored_its_ticket`, `no_reclaim_happened`, `reseat_issued` (`no ledgered survivors; ... host_reseats_without_survivors=1`).

Breadth extras (`D:\mx\s41b3\breadth.json:14295-14305`): `B1 running activity is not evidenced`, `B1 did not adjudicate exactly one dropped seat`, `B1 substitutions_committed is not exactly one`.

### Intended sequence (plan + gate)

`STAGE2_H4_RECONNECT_PLAN.md` §8/§9b: drop records a deterministic ownership ledger; host approval draws a credential without installing it; first COMMIT wins; substitute receives ledgered ownership from resumed tick 1; never “release seat then let a substitute join”.

Gate (`h4_substitution_gates.py:5-7, 170-176, 275-296, 331-366`): 3-peer match, kill leaver at 22s, wait for `left the match at frame`, start applicant with `-net-h4-apply 1`, host `-net-h4-substitute 1 -net-h4-substitute-delay 0`, then require admission counters + substitute `Joined` + ticket stored.

### Actual sequence (ticks/frames)

Host `stdout.log:8-18` / `runtime\LogConsole.txt:8-16`:

1. Wait / drop: `Client left the match at frame 993 (connection lost)`; stall recovered tick 993.
2. Console: `NETWORK: Holding the match open for a player who can still return` (`ActivityMan.cpp:1015`).
3. `moderation: substitute seat 1 -> Ok` (`DriveAutoSubstitution`, `NetMatchService.cpp:1130-1132`).
4. `rejoin: Client reconnected - resyncing the match` (`NetMatchService.cpp:775-776`).
5. `resync: requested, reloading from the host snapshot`.
6. `snapbench save ... saved=1` (not the Source40 save-failed residual).
7. `launching from the received snapshot: p5resync_23984`; `resync: match relaunched from the snapshot`.
8. Console: activity reset / Grasslands / started / **ended**.
9. `[net-match-service-e2e] wrote report`.

Stayer `stdout.log:6-15` / `LogConsole.txt:9-15`: same drop at 993; `resync: requested`; state transfer 5434291 bytes; relaunch `p5resync_23852_recv`; then ended.

Substitute `stdout.log:4-9` / `LogConsole.txt:6-11`: no first-match drop (process started after adjudication); state transfer; lobby as peer2/team1; launch `p5resync_24672_recv`; `Holding the match open`; ended. Report: `client_state=Joined`, `client_commits=1`, `client_applications_acknowledged=1`, `client_substitution_offers=1`, `client_substitution_acks=1`, `ticket_stored=true`.

Leaver `stdout.log`: lobby then killed (`verdict.records.leaver.exit_code=137`).

No `Rejected a` / `AbortLog`. No `[net-reconnect] reseating team` line (matches `host_reseats_issued=0`).

### Why host and stayer exit 1

After relaunch, e2e tick accounting is zeroed:

```2112:2117:D:\Projects\p4b-interp-validation\Source\Main.cpp
			if (resyncOk) {
				std::cout << "[net-match] resync: match relaunched from the snapshot" << std::endl;
				if (s_netMatchServiceE2E) {
					s_netMatchServiceE2EStartTick = UINT64_MAX;
					s_netMatchServiceE2ERunningTicks = 0;
				}
```

Then the first post-relaunch Over with `running_ticks < 100` is treated as a broken setup:

```2933:2940:D:\Projects\p4b-interp-validation\Source\Main.cpp
				if (activityState == Activity::HasError || (activityState == Activity::Over && s_netMatchServiceE2ERunningTicks < 100)) {
					s_netMatchServiceE2EError = std::string("activity ended in state ") + ActivityStateName(activityState);
					s_netMatchServiceE2EExitCode = 1;
					g_NetMatchService.ReportRuntimeError(s_netMatchServiceE2EError);
					System::SetQuit(true);
					break;
				}
```

`RunNetMatchServiceE2E` returns that code (`Main.cpp:3736-3738`). Host/stayer reports: `runtime_error="activity ended in state Over"`, `running_ticks=0`, `resyncs=1`, `exit_code=1`. Console `SYSTEM: Activity "P4 Alpha Duel" was ended` is `ActivityMan.cpp:1023-1024` after the reclaim-hold no longer defers (`ActivityMan.cpp:1011-1018`).

### Why admission fields are `None`

Host report after that path (`evidence/report_fields.json` commit.host): `has_runner=false`, `admission=null`, `is_host=false`, `admission_attached=false`, `moderation=[]`, `seats=[]`. `ReportRuntimeError` ran before `BuildNetMatchServiceE2EReportJson`. The gate’s `admission_of` therefore returns `{}` and `.get(...)` is `None`.

The writer that *would* have filled those keys (`NetSession.cpp:1078-1128`) did not run because `m_Session` had been moved out.

---

## 2. `substitute_returner_wins`

### Failing checks (quoted)

`D:\mx\s41b3\j44\substitute_returner_wins_20260910_023039\verdict.json:18-76`:

- `host_exit_zero` fail `1`
- `stayer_exit_zero` fail `1`
- `seat_dropped` fail `None`
- `returner_reclaimed` fail `None`
- `seat_has_one_holder` fail `None`

Passing: `host_adjudicated_the_drop`, `returner_reseat_decided` (`no ledgered survivors; host_reseats_without_survivors=1`), `returner_joined Joined`, `substitution_did_not_commit` (detail `None`, and `(None or 0)==0` is a pass), `substitute_not_joined Idle`.

### Intended sequence

Plan §9b: returner-vs-substitute, first COMMIT wins; a reclaim in `BindIncarnation` abandons an in-flight substitution (`NetReconnectSession.cpp:572-576`). Gate (`h4_substitution_gates.py:177-184, 229-235, 367-396`): same drop, then returner (same `leaver.ticket`) starts before the applicant; host `-net-h4-substitute-delay 9000` so the reclaim lands first; require `reclaims_accepted>=1`, substitute not `Joined`, `incarnations_bound>=1`.

### Actual sequence

Host `stdout.log:8-17` / `LogConsole.txt:8-16`:

1. Drop at frame 992; stall recovered tick 992.
2. `Holding the match open...`
3. **No** `moderation: substitute seat` line (9000 ms delay never elapsed).
4. `rejoin: Client reconnected - resyncing the match`.
5. Snapshot `saved=1`; relaunch `p5resync_10056`; activity started then ended.
6. Report written.

Stayer: drop 992; first activity Holding then ended (`LogConsole.txt:9-11`); then `Resyncing ... tick 1327`; relaunch; Holding; ended.

Returner `stdout.log:4-9`: state transfer 5346980 bytes; lobby as peer2/team1; launch `p5resync_4012_recv`. Report: `client_state=Joined`, `client_commits=1`, `client_used_stored_ticket=true`.

Substitute `stdout.log:4-5`: `setup failed: transport stopped`. Report: `client_state=Idle`, `client_applications_sent=0`, `client_commits=0`. `verdict.records.substitute.exit_code=1` (setup path, `Main.cpp:3732-3736`).

### Why host and stayer exit 1

Same `Main.cpp:2114-2117` + `2935-2940` path as commit. Host/stayer reports: `runtime_error="activity ended in state Over"`, `running_ticks=0`, `resyncs=1`, `exit_code=1`.

### Why admission fields are `None`

Same Failed teardown. Host `has_runner=false`, `admission=null`. `substitution_did_not_commit` still passes because a missing `substitutions_committed` is treated as 0.

---

## 3. Tool vs engine for each `None`

| Field | Gate reads | Engine emits on Complete (cancel/bounds) | Present on these Failed reports | Class |
|---|---|---|---|---|
| `seats_dropped` | `admission.seats_dropped` | yes, and also `reconnect.host_seats_dropped` | admission absent; **`reconnect.host_seats_dropped=1`** on both hosts | **tool-side** (surviving twin ignored). Drop itself ran. |
| `applicants_registered` | `admission.applicants_registered` | yes (`cancel=1`, `bounds=2`). Not copied into `reconnect` | absent | **tool-side** for the `None` (Failed writer never serializes this counter). Registration ran on commit: host `substitute seat 1 -> Ok` requires `FindApplicant`; substitute `client_applications_acknowledged=1`. Not an “applicant never registered” engine miss. |
| `substitutions_committed` | `admission.substitutions_committed == 1` | yes (`cancel=0`, `bounds=0`). Increment at `NetReconnectSession.cpp:1027`. Not copied into `reconnect` | absent | **tool-side** for the `None`. Commit ran: substitute `Joined` / `client_commits=1` / offer+ack+ticket. Not “substitution not committed” on the wire. |
| `reclaims_accepted` | `admission.reclaims_accepted` | yes (`cancel=0`). Increment at `NetReconnectSession.cpp:500`. Not copied into `reconnect` | absent | **tool-side** for the `None`. Reclaim ran: returner `Joined` + `client_used_stored_ticket=true`; substitute `Idle` / no apply; no host approve line. |
| `incarnations_bound` | `admission.incarnations_bound` | yes (`cancel=2`, `bounds=2`). Increment in `BindIncarnation` (`:587`) | absent | **tool-side** for the `None`. Bind ran if reclaim/commit ran (`IssueReseat` counted `reseats_without_survivors=1`). |

Engine-side facts that are **not** the `None`s, but are why the Failed writer ran:

- After a successful commit/reclaim the new Ready peer has no lockstep remote → `RequestResync` (`NetMatchService.cpp:772-777`).
- Relaunch zeroes `running_ticks`; deferred scripted Over then trips the `<100` e2e check.
- `IssueReseat` (`NetReconnectSession.cpp:672-676`) recorded `reseats_without_survivors=1` (ledger existed, no living UIDs). Plan §8: survivors only; otherwise lose per the win condition.

Source40 lanes table (`reviews/claude-review-2026-09-08/lanes/source33-lanes/report.md:2108-2117`) named the same `None`s as “ended-activity report shape”. This family’s hosts differ from that paragraph’s `resync snapshot save failed`: here `saved=1` (`TRIAGE.md` §7–8; `report.md:2262` “hosts survive the resync snapshot save”). Residue that remains is Over-after-relaunch + admission tree torn down.

---

## 4. At most three mechanisms (no fixes)

### M1 — Failed-path teardown hides admission

Must execute: `Main.cpp:2935-2938` `ReportRuntimeError` → `NetMatchService.cpp:503-518` move runner/session, `EndAdmissionSession` → `BuildReportJson:928-930` skips `runner` → gate `admission_of` returns `{}`.

Confirming observation: failing hosts `has_runner=false`, `admission=null`, `is_host=false`; passing hosts `has_runner=true` and `admission.seats_dropped=1`. Same exe.

Detecting control: host report must contain `service.runner.session.admission.seats_dropped` as an integer, **or** the gate must read `service.reconnect.host_seats_dropped` (already `1` on both red hosts). Fail if admission is missing after a live drop.

### M2 — Commit/reclaim → resync → `running_ticks=0` → Over is scored as setup failure

Must execute: `NetMatchService.cpp:772-777` `RequestResync("player rejoined")` → `Main.cpp:2074-2117` snapshot+relaunch+zero ticks → `ActivityMan.cpp:1006-1024` End no longer deferred once the seat is bound / the new coordinator has committed a frame (`ScenarioRunner.cpp:796-804`) → `Main.cpp:2935-2940` exit 1.

Confirming observation: both red hosts `resyncs=1`, `saved=1`, `running_ticks=0`, `runtime_error="activity ended in state Over"`. Both green hosts `resyncs=0`, `running_ticks=3601`, `exit_code=0`, same `activity_state=Over` / `winner_team=2` at the cap. All four launch argvs include `-net-match-e2e-resync`.

Detecting control: after `resyncs>=1`, either `running_ticks>=100` before Over, or the `<100` Over check must not fire when the console already printed `Holding the match open` on the pre-resync round. Fail if relaunch + immediate Over is the only reason `exit_code=1`.

### M3 — Reseat runs, ledger names no living actors

Must execute: drop → `RecordDrop` / `ledgerDropsRecorded++` → commit or reclaim → `IssueReseat` (`NetReconnectSession.cpp:645-676`) → `restored.empty()` → `reseatsWithoutSurvivors++`, no `EnqueueLocalGameCommand` reseat.

Confirming observation: both red hosts `host_ledger_drops_recorded=1`, `host_reseats_without_a_ledger=0`, `host_reseats_without_survivors=1`, `host_reseats_issued=0`; host logs have no `reseating team`. Green hosts have `host_reseats_without_survivors=0` (no commit/reclaim, so `IssueReseat` never ran).

Detecting control: after a commit or reclaim, assert either `host_reseats_issued>=1` and a `reseating team` line, **or** an explicit `reseats_without_survivors>=1` plus a still-running match (`running_ticks` to cap). Fail if “no survivors” is indistinguishable from “reseat never attempted” once admission is torn down.

---

## RESUME.md hits (truncated 600 chars)

From `evidence/resume_hits.txt` (needles `B1`, `substitution`, `substitute_commit`, `returner_wins`):

- L63: long-red H4 gates then `B1 substitution 2/4`.
- L1300: slice B1 implemented; “two crash today on the resync snapshot save” (this family is `saved=1`, then Over).
- L1295: B1 snapshot fix still open against A6.
- Other hits are older handoff / render-substitution lines; full text in `evidence/resume_hits.txt`.

---

## Not done

No engine launch, no build, no edits outside this directory. Did not inspect in-process `m_Stats` after teardown (no debugger). Did not open the `.ccsave` bytes to see whether the snapshot itself was already Over.
