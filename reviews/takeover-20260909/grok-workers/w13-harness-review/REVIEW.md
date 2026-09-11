# W13 independent review — Source41 breadth-wrapper changes

Input: `w4-breadth-harness/harness.diff`, `REPORT.md`, `w2-breadth-triage/{TRIAGE,LEAD_VERDICTS}.md`, `source33-lanes/report.md` Source38–40, evidence roots `D:\mx\s41b4`, `D:\mx\s41b4-extra2`, `D:\mx\s41b5`. Did not author the changes.

Command that produced the adversarial results: `python D:\Projects\reviews\takeover-20260909\grok-workers\w13-harness-review\probe.py` → `probe.json` (imports `D:\Projects\reviews\takeover-20260909\run_breadth.py`).

---

## 1. compat_deferral uid exclusion

### (a) Scope of the exclusion

`SPAWN_CHILD_LINE` and `compat_lines_match`:

```
D:\Projects\reviews\takeover-20260909\run_breadth.py:1179
SPAWN_CHILD_LINE = re.compile(r"^PRINT: \[deferral\] case=spawn_child ok=1 uid=\d+$")
```

```
D:\Projects\reviews\takeover-20260909\run_breadth.py:1206-1215
def compat_lines_match(actual, expected) -> bool:
    if not isinstance(actual, list) or not isinstance(expected, list) or len(actual) != len(expected):
        return False
    for got, want in zip(actual, expected):
        if SPAWN_CHILD_LINE.match(want):
            if not SPAWN_CHILD_LINE.match(got):
                return False
        elif got != want:
            return False
    return True
```

Exactly one field of one line is excluded: the digits after `uid=` on a line that otherwise must be `PRINT: [deferral] case=spawn_child ok=1 uid=<digits>`. Line count (`len`), order (`zip`), `ok=1`, case name, and every other line (character-exact) remain compared. `cases` is still exact (`run_breadth.py:1786`). The pin still contains one matching line (`run_breadth.py:913`).

Old check was `row.get("lines") == reference["lines"]` (`pre/run_breadth.py:1708`).

Adversarial calls (`probe.json` `compat_cases`; imported `compat_lines_match`):

| input | old rejects | new rejects |
|---|---|---|
| identical | no | no |
| uid-only `1049517`→`1049513` | yes | **no** (the intended exclusion) |
| `ok=0` | yes | **yes** |
| reordered lines | yes | **yes** |
| changed non-uid line (`step=149`) | yes | **yes** |
| spawn_child + trailing ` extra` | yes | **yes** |
| missing spawn line / extra line / `uid=abc` / wrong case name | yes | **yes** |

### (b) Cited evidence

```
D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\report.md:1531-1534
the `spawn_child` UniqueID counter, `uid=1049494` (reference) against
`uid=1049517` (approved), `ok=1` on both. Rather than report that as a build difference I repeated
the probe, and the reference itself moved: **`source22` printed `1049494` in run 1 and `1049517` in
run 2**
```

```
report.md:1574
the only line that ever differed was the `spawn_child` UniqueID counter, which a repeat showed varies run to run on the reference executable itself (1049494 then 1049517)
```

Yes: the same Source22 executable printed two different uids across two runs. That is field-specific evidence that the counter is not build signal.

### (c) What a real spawn regression would still hit

Covered: `ok=0`; missing/extra/reordered lines; any other PRINT character; `cases` dict; fixture `say("spawn_child", true, "uid=" .. child.UniqueID)` (`sound_ai_deferral.lua:348`) still requires a successful create+`AddActor` to print `ok=1`; console `ERROR:` still scanned (`run_breadth.py:1170-1177`).

Not covered: the numeric uid; a spawn that still prints `case=spawn_child ok=1 uid=<digits>` for a different object that happens to have a UniqueID. `spawn_first_ai` (`created=1` …) remains exact and is a separate case.

**Verdict: ACCEPT.** One field of one line; cited evidence shows the same exe moving the counter; adversarial rejects still fire.

---

## 2. fake-lag oracle

### (a) Formula vs engine

```
D:\Projects\p4b-interp-validation\Source\Network\NetMatchRunner.cpp:79-89
		if (config.host && config.autoInputDelay) {
			const double tickMs = 1000.0 / 30.0;
			const uint16_t floorDelay = m_MatchConfig.inputDelayFrames;
			std::vector<uint16_t> delays(m_MatchConfig.peerCount, std::max<uint16_t>(floorDelay, 1));
			for (const auto& [peerId, transportId]: BuildRemoteTransportMap(session)) {
				const uint32_t rttMs = transport.GetPeerPingMs(transportId);
				const uint16_t neededDelay = static_cast<uint16_t>(std::min<uint32_t>(
				    static_cast<uint32_t>(std::ceil(rttMs / tickMs)) + 1U, NetMatchConfigUtil::c_MaxInputDelayFrames));
				delays[peerId - 1] = std::max(delays[peerId - 1], neededDelay);
				std::cout << "[net-match] auto input delay: peer " << static_cast<int>(peerId) << " rtt " << rttMs
				          << "ms -> " << delays[peerId - 1] << " frames (manual floor " << floorDelay << ")" << std::endl;
```

Wrapper (`run_breadth.py:1218-1220`): `int(math.ceil(rtt_ms / (1000.0 / 30.0))) + 1`. Same `ceil(rtt / (1000/30)) + 1`. Cap `c_MaxInputDelayFrames = 60` (`NetMatchConfig.h:56`) is not mirrored; in the fl100/fl200 band the result is 7–14, so the missing min is inert. `probe.json` `old_pin_rtt_to_frames`: `400/(1000/30) = 12.0` exactly → ceil 12 + 1 = 13; `401/tick = 12.03` → 14. Rounding matches the C++ `+ 1U`.

### (b) Strictness vs the old integer pin

Old pin (`pre/run_breadth.py` / TRIAGE.md:38-39): `delay = 8 if fl100 else 14` and `peer_input_delays == {"1":1,"2":delay}`. No log parse, no RTT check, no formula check.

Implicit RTT that produces those frames under the engine formula (`probe.json` `formula_rows`):

- fl100 old accepted frames=8 ⇒ integer RTT **201–233** (and any other RTT the engine mapped to 8; RTT itself was unchecked).
- fl200 old accepted frames=14 ⇒ integer RTT **401–433**.

New accepts (`run_breadth.py:1225-1251`): host log present; `frames == ceil(rtt/tick)+1`; `rtt ∈ [2*lag, 2*lag+15]`; both `peer_input_delays == {"1":1,"2":<logged frames>}`. Optional client log, if present, must match host rtt/frames.

So new is **stricter on engine math and peer agreement**, and **stricter on RTT being near 2×lag** (old would accept rtt=233→8 or rtt=433→14). New is **weaker on the absolute frame count**: fl200 at rtt=400→13 and fl100 at rtt=200→7 now pass.

Residual hole: a hardcoded delay of 13 with measured rtt=400 satisfies formula+band+maps (`probe.json` `hardcoded_13_at_400` rejects=false). At any other rtt in [401,415] that hole closes.

### (c) Measured RTTs (every quote found)

`report.md` (Source38–40):

```
report.md:1644-1645
fl100: auto input delay: peer 2 rtt 200ms -> 7 frames   (Source33-38: rtt 201ms -> 8 frames)
fl200: auto input delay: peer 2 rtt 401ms -> 14 frames  (unchanged)
```

```
report.md:1992-1993
auto delays back to **8 and 14** (Source39 read 7 at fl100 because its measured RTT
landed at 200 ms rather than 201; this run measured the usual value):
```

Source38 body quotes delays `{1:1,2:8}` / `{1:1,2:14}` (`report.md:1368`) but not a raw RTT; the 201 ms figure for Source33–38 is the Source39 sentence above. Source40 quotes frames 8 and 14, not a raw RTT.

Host logs:

```
D:\mx\s41b3\j27\fl\fl100\host\stdout.log:5
[net-match] auto input delay: peer 2 rtt 201ms -> 8 frames (manual floor 0)
D:\mx\s41b3\j28\fl\fl200\host\stdout.log:5
[net-match] auto input delay: peer 2 rtt 400ms -> 13 frames (manual floor 0)
D:\mx\s41b4\j27\fl\fl100\host\stdout.log:5
[net-match] auto input delay: peer 2 rtt 202ms -> 8 frames (manual floor 0)
D:\mx\s41b4\j28\fl\fl200\host\stdout.log:5
[net-match] auto input delay: peer 2 rtt 400ms -> 13 frames (manual floor 0)
```

Quoted set: fl100 **200, 201, 202**; fl200 **400, 401**. Span above `2*lag` is **0–2 ms**. The **+15 ms** band is not implied by that data.

### (d) Adversarial failures (`probe.json` `lag_cases`)

All reject as required: `400→14` (formula), `401→13` (dropped +1), rtt 399 / 416 (band), host/client map disagree, client log disagree, host line missing. Measured s41b4 400→13 and 202→8 accept.

**Verdict: ACCEPT WITH NOTE.** Formula matches `NetMatchRunner.cpp:80-86` including ceil and +1, and is stricter about engine arithmetic than the old pin. The `[2*lag, 2*lag+15]` tolerance is a new widening; measured RTTs only support +0–2 ms. Lead can keep the formula+maps and tighten the band.

---

## 3. compat_extra `HasAnySounds(true)` and pin recapture

### (a) Binding and default

```
D:\Projects\p4b-interp-validation\Source\Lua\LuaBindingsEntities.cpp:1358
	    .def("HasAnySounds", &SoundSet::HasAnySounds)
```

```
D:\Projects\p4b-interp-validation\Source\Entities\SoundSet.h:145
		bool HasAnySounds(bool includeSubSoundSets = true) const;
```

SoundContainer no-arg binding is separate (`LuaBindingsEntities.cpp:1325`). Luabind sees `HasAnySounds(bool)`; the C++ default is not applied. `HasAnySounds(true)` is the default the author wrote in C++. Live fixture: `compat-review/fixtures/compat_review_extra.lua:97`. Pre: `pre/compat_review_extra.lua:97` `HasAnySounds()`.

### (b) Both rungs and pin

`probe.json` `extra_compare` (from `D:\mx\s41b4\j80\summary.json` and `j81\summary.json`):

- `j80_vs_j81_cases` true, `j80_vs_j81_lines` true, 19/19, diffs `[]`
- `pin_cases_eq_j80` true, `pin_lines_eq_j80` true (also equals j81)
- fixture sha `e92fa5fb5095b4cec374a6f7ef69c3559edb7028e6ed2fbc00a2fe214eccfaaf` on pin, j80, j81

`j80\source22\runtime\LogConsole.txt` and `j81\approved\runtime\LogConsole.txt`: no `ERROR:` (grep on those two files). Same for `trace.json.console.txt`. Extra2 per-case `passed=true` `failures=[]` (`D:\mx\s41b4-extra2\breadth.json:15-20`, `:59-64`).

Pin recapture is legitimate under the brief: Source22 and approved agree on the same fixture.

### (c) `shared_soundset_structural before=1 removed=0 any=1`

Fixture (`compat_review_extra.lua:94-101`): `xSet` was built with SHORT (`:58`); then `HasAnySounds(true)`, `AddSound(LONG)`, `RemoveSound(LONG)`, `s:HasAnySounds()`.

`GetTopLevelSoundSet` is a live reference (`SoundContainer.h:166-169`, binding `return_internal_reference`). Shared `Update` runs under `SoundSimulationScope` default **SharedSimulation** (`SoundSimulation.h:22`, `MovableMan.cpp:3969-3973`), so `SoundSet::DeferringOwner` is null (`SoundSet.cpp:198-200`) and `AddSound`/`RemoveSound` take the Now path, not the AI queue.

One-arg `RemoveSound` does not search sub-sets (`SoundSet.h:112`: `RemoveSound(path, false)`). LONG is added to the same top-level `m_SoundData` (`SoundSet.cpp:309-316`), so if `AddSoundNow` landed, `RemoveSoundNow` should return true (`removed=1`). Observed `removed=0` on **both** rungs (`j80`/`j81` `summary.json:33` and `:51`). That is identical pre-change-rung behaviour, not an approved-only quirk.

Likely: `AddSoundNow` returned without pushing (`!soundObject`, `SoundSet.cpp:312-314`, abort flag false from the one-arg wrapper `SoundSet.h:84`) or `GetDataPath() == soundFilePath` failed in `RemoveSoundNow`. Not expected “add then remove succeeded”. Record as a **quirk**: the new pin exact-compares `removed=0`, so a later `removed=1` would fail the gate. `any=1` only proves SHORT (or something) remains; it does not prove LONG was added then stripped.

**Verdict: ACCEPT WITH NOTE.** Binding/default confirmed; both-rung capture equals the pin; `removed=0` is a recorded quirk identical on Source22, not proof that RemoveSound found LONG.

---

## 4. heal child input delay 0 → 3

Source40 heal:

```
D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates\20260909_112620_heal_26d39dc2\result.json:10
  "input_delay": 3,
```

Host argv includes `-net-match-input-delay` `3` (`result.json:24-25`). `report.md:2192-2193` names that evidence directory. Source39 command was `heal --input-delay 3` (`report.md:1868`).

Required-check list: `pre/run_breadth.py:739-763` vs `run_breadth.py:740-764` — same 23 names in the same order (`probe.json` `heal.pre_vs_fixed_heal_list_equal` true). Only `INPUT_DELAY` changed (`pre:1883` `= 0` → `run_breadth.py:1964` `= 3 if heal else 0`). Invariance stays 0.

s41b5 heal at delay 3 passed including `prediction_executed` (`D:\mx\s41b5\j73\result.json:10` `input_delay=3`; `D:\mx\s41b4` at delay 0 missed `prediction_executed`).

**Versus Source40: equal** (same delay, same check names). Versus the pre wrapper: stricter, because delay 0 could not satisfy `prediction_executed`.

**Verdict: ACCEPT.**

---

## 5. `--cases` filter and `sys.path` insert

```
D:\Projects\reviews\takeover-20260909\run_breadth.py:1193-1195
def select_jobs(jobs: list[Job], cases: list[str] | None) -> list[Job]:
    if not cases:
        return list(jobs)
```

```
run_breadth.py:2005-2013
    jobs = plan(...)
    if tuple(job.id for job in jobs) != REQUIRED_CASES or len(REQUIRED_CASES) != 81:
        raise RuntimeError("breadth coverage inventory is incomplete")
    try:
        jobs = select_jobs(jobs, options.cases)
```

`probe.json` `select_jobs.none_returns_same_ids` true (81 ids, same order, a copy). `--cases` absent ⇒ `options.cases is None`.

`driver_inputs` (`run_breadth.py:1939-1951`) unions (1) `.py`/`.ps1` args from **selected** jobs with (2) a fixed set (SCRIPT, recovery_e2e, all four compat/deferral fixtures, tools, …). A full 81-job selection includes every job script the pre wrapper hashed. The pin record for a full run does not change. A `--cases` subset hashes fewer job scripts into `drivers.json`; that is subset-rerun bookkeeping, not a full-run semantic change.

`sys.path.insert(0, str(STAGE))` is only in `child()` (`run_breadth.py:1966`), after INPUT_DELAY, before `heal()`/`invariance()`. The parent 81-case validator never takes that path.

**Verdict: ACCEPT.** No effect on full 81-case comparison semantics.

---

## Verdicts

| # | change | verdict |
|---|---|---|
| 1 | compat_deferral uid exclusion | ACCEPT |
| 2 | fake-lag formula+band oracle | ACCEPT WITH NOTE |
| 3 | compat_extra bool + pin recapture | ACCEPT WITH NOTE |
| 4 | heal delay 0 → 3 | ACCEPT |
| 5 | `--cases` / `sys.path` | ACCEPT |

No REJECT.

Notes the lead must still own: (2) +15 ms band vs measured +0–2 ms, and the rtt=400 / frames=13 hardcoded-delay hole; (3) `removed=0` is a both-rung quirk, not a successful RemoveSound.
