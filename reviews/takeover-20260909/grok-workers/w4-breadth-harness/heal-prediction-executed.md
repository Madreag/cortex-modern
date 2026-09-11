# Heal `prediction_executed` (s41b4, read-only)

Commands: grep of the named trees; `python grok-workers/w4-breadth-harness/search_heal_prediction.py` (reparse-safe, skip `Data`/`external`/`modules`, `*.json`/`*.log`/`*.txt` ≤ 20 MB) → `heal-prediction-search.json`.

## 1. Wrapper requirement

Heal family required-check list (`run_breadth.py` `BASELINE["heal"]`):

```740:764:D:\Projects\reviews\takeover-20260909\run_breadth.py
  "heal": [
    "host_process_exit",
    "host_report_present",
    "client_process_exit",
    "client_report_present",
    "host_liveness",
    "host_lockstep_completed",
    "host_no_editor",
    "host_pace_recorded",
    "host_resynced",
    "client_liveness",
    "client_lockstep_completed",
    "client_no_editor",
    "client_pace_recorded",
    "client_resynced",
    "config_agreement",
    "census_agreement",
    "scenario_identity_recorded",
    "prediction_counters_recorded",
    "prediction_executed",
    "host_script_continued",
    "host_no_script_errors",
    "client_script_continued",
    "client_no_script_errors"
  ],
```

Heal validator applies that list with no `allowed_na`:

```1765:1768:D:\Projects\reviews\takeover-20260909\run_breadth.py
        elif family == "heal":
            data = v.json(root / "result.json")
            v.checks(data, BASELINE["heal"])
            v.require(data.get("complete") is True and data.get("source_unchanged") is True, "heal incomplete or changed source")
```

`required_checks_present` is **not** used for heal. It is an interp-only named check (`BASELINE["interp"][*]` and `run_interp_e2e.ps1:138-140`). Heal missing-name logic is `Verdict.checks`:

```1462:1464:D:\Projects\reviews\takeover-20260909\run_breadth.py
        seen = Counter(c.get("name") for c in rows)
        for name, count in Counter(required).items():
            self.require(seen[name] >= count, f"missing required check {name} x{count}")
```

Interp is the family that allows `prediction_executed` as `n/a`:

```1709:1709:D:\Projects\reviews\takeover-20260909\run_breadth.py
            v.checks(data, BASELINE["interp"][name], allowed_na=("prediction_executed",))
```

s41b4 heal child result `D:\mx\s41b4\j73\result.json` (`input_delay` 0 at `:10`). Check names actually present (all `status=pass`):

| name | detail |
|---|---|
| host_process_exit | `exit_code=0 timed_out=False elapsed=22.851` (`:247`) |
| host_report_present | `host_report.json` (`:257`) |
| client_process_exit | `exit_code=0 timed_out=False elapsed=21.731` (`:265`) |
| client_report_present | `client_report.json` (`:275`) |
| host_liveness | `running_ticks=601` (`:281`) |
| host_lockstep_completed | `timeout_reason='Complete:e2e complete'` (`:289`) |
| host_no_editor | `entered_editor=False` (`:297`) |
| host_pace_recorded | `wall_tps=47.8636` (`:305`) |
| host_resynced | `resyncs=1` (`:313`) |
| client_liveness | `running_ticks=600` (`:321`) |
| client_lockstep_completed | `Complete:e2e complete` (`:329`) |
| client_no_editor | `entered_editor=False` (`:337`) |
| client_pace_recorded | `wall_tps=47.9042` (`:345`) |
| client_resynced | `resyncs=1` (`:353`) |
| config_agreement | hashes equal (`:361`) |
| census_agreement | `host actors=2 client actors=2` (`:369`) |
| scenario_identity_recorded | `P4 Alpha Duel` / `Grasslands` (`:379`) |
| prediction_counters_recorded | `enabled=True previews=0 actor_ticks=0 taken=0 violations=0` (`:387`) |
| host_script_continued | pass (`:413`) |
| host_no_script_errors | `errors=[]` (`:413`) |
| client_script_continued | pass (`:418`) |
| client_no_script_errors | `errors=[]` (`:434`) |

`prediction_executed` is absent from this list. Wrapper failure: `D:\mx\s41b4\breadth.json` heal `failures=["missing required check prediction_executed x1"]`. `D:\mx\s41b4\logs\heal.log:1`: `resync_heal PASS []`.

## 2. Who produces `prediction_executed`

Grep: `recovery_expanded_mod.py` 0 hits. `p4b-interp-validation\tools\` 0 hits. `p4b-interp-validation\Source\` 0 hits.

### Producer A — `recovery_e2e.py` (heal / e2e lanes)

```466:474:D:\Projects\stage2_p4\recovery_e2e.py
            if spec.get("delay", 0) > 0 or spec.get("expect_prediction"):
                check(
                    "prediction_executed",
                    bool(lp.get("enabled"))
                    and lp.get("previews", 0) > 0
                    and lp.get("violations", 1) == 0,
                    f"previews={lp.get('previews')} violations={lp.get('violations')}",
                    [host_report],
                )
```

`LANES["resync_heal"]` default `delay=0` (`recovery_e2e.py:187-189`). `recovery_expanded_mod.py:130-131` copies that spec then `spec['delay'] = INPUT_DELAY`. Breadth `child()` sets `module.INPUT_DELAY = 0` (`run_breadth.py:1963`). So the heal child never takes the `if` and never appends the check.

`expect_prediction=True` is only on the fake-lag lane (`recovery_e2e.py:150`), not heal.

### Producer B — `run_interp_e2e.ps1` (interp matrix, not heal)

Definition / emit:

```765:771:D:\Projects\stage2_p4\run_interp_e2e.ps1
        if (-not $predictionOff -and $previews -le 0) {
            Add-Check "prediction_executed" "fail" "$($run.Name) ran with input delay $delayFrames but previewed $previews frames"
        } elseif (-not $predictionOff) {
            Add-Check "prediction_executed" "pass" "$($run.Name) delay=$delayFrames previews=$previews ..."
```

```801:807:D:\Projects\stage2_p4\run_interp_e2e.ps1
if ($predictionOff -and $RequirePrediction) {
    Add-Check "prediction_executed" "fail" ...
} elseif ($predictionOff) {
    Add-Check "prediction_executed" "n/a" "prediction disabled by -net-local-prediction off (control run)" $false
} elseif ($delayedPeers.Count -eq 0) {
    if ($RequirePrediction) { Add-Check "prediction_executed" "fail" "no peer ran with an input delay, so prediction never executed" }
    else { Add-Check "prediction_executed" "n/a" "D=0: regression-only run; prediction not exercised" $false }
}
```

`Get-RequiredChecks` includes `"prediction_executed"` for ordinary interp modes (`run_interp_e2e.ps1:107`). `required_checks_present` then asserts those names were recorded (`:138-140`).

### Consumers

- `run_breadth.py` `BASELINE["heal"]` (unconditional) and `BASELINE["interp"]` / `BASELINE["fakelag"]` (interp via `allowed_na`).
- `run_breadth.py:1709` interp `allowed_na=("prediction_executed",)`.
- `test_run_breadth.py:112-117` (synthetic `allowed_na` test).
- Source40 copy of the same heal list: `source40-evidence-review\breadth-baseline.json:667-686`.

## 3. Source38–40 baseline

`D:\mx\s40lanes` reparse-safe walk: 35 `prediction_executed` hits, all interp / semantic / fake-lag (`fl\fl100`, `fl\fl200`, `mx\*`, `sem\*`). **Zero heal jobs under `s40lanes` contain the string.**

Source38–40 report (`source33-lanes\report.md`):

- Interp: `prediction_executed` is `n/a` when D=0 (`report.md:150-151`, `:360`, `:498`).
- Heal Source38 table: `Two-peer heal | **PASS**` (`report.md:1616`) — counters only, no check-name list.
- Heal Source39: command `python D:/Projects/stage2_p4/recovery_expanded_mod.py heal --input-delay 3` (`report.md:1868`). Evidence `20260909_041847_heal_bc723df9`.
- Heal Source40: `resync_heal PASS []` (`report.md:2190-2193`). Evidence `20260909_112620_heal_26d39dc2`.

Those two retained heal results **did** emit the check (delay 3):

```10:10:D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates\20260909_041847_heal_bc723df9\result.json
  "input_delay": 3,
```

```401:405:D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates\20260909_041847_heal_bc723df9\result.json
      "name": "prediction_executed",
      "status": "pass",
      "detail": "previews=657 violations=0",
```

```10:10:D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates\20260909_112620_heal_26d39dc2\result.json
  "input_delay": 3,
```

```403:407:D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates\20260909_112620_heal_26d39dc2\result.json
      "name": "prediction_executed",
      "status": "pass",
      "detail": "previews=657 violations=0",
```

The Source41 wrapper heal list is the delay-3 check set (same names as those two files, also frozen in `breadth-baseline.json:667-686`). The wrapper heal **invocation** is delay 0 (`child()` `INPUT_DELAY = 0`), which is not the Source39/40 heal command.

## 4. Prediction signal on both heals

Field: `local_prediction` on the match report (not `predicted_frames` / `rollbacks`; those names are absent).

s41b4 heal (delay 0), host `D:\mx\s41b4\j73\fresh\e2e\resync_heal\host_report.json` (single-line JSON): `local_prediction={"enabled":true,"previews":0,"actor_ticks":0,"ms_total":0,"shadows":0,"taken":0,"violations":0}`. Same object echoed as check `prediction_counters_recorded` `result.json:387-389`. Client report: `local_prediction={"enabled":true,"previews":0,...}`.

Source40 heal (delay 3), host `...\20260909_112620_heal_26d39dc2\fresh\e2e\resync_heal\host_report.json`: `local_prediction={"enabled":true,"previews":657,"actor_ticks":1971,"ms_total":2930.58,"shadows":0,"taken":0,"violations":0}`. Same numbers in `result.json:397` and `prediction_executed` `previews=657` (`:405`).

Source39 heal host report: `previews=657 actor_ticks=1971` (`20260909_041847_heal_bc723df9\fresh\e2e\resync_heal\host_report.json`).

Prediction **ran** on Source39/40 heal (delay 3, 657 previews). Prediction **did not preview** on s41b4 heal (delay 0, 0 previews) while remaining `enabled=true`. That matches `recovery_e2e.py:466` (no `prediction_executed` row) and the interp D=0 `n/a` contract (`run_interp_e2e.ps1:807`).

## 5. Classification proposal

**HARNESS_REQUIREMENT_WITHOUT_PRODUCER**

The wrapper demands `prediction_executed` on heal (`BASELINE["heal"]` + `v.checks` with no `allowed_na`) while the heal child it launches sets `INPUT_DELAY = 0`, so `recovery_e2e.py:466-474` does not emit the check. `tools/` and `Source/` have no producer. Source40/39 heal **did** emit the check, but only because those runs used `--input-delay 3`. The s41b4 engine report is consistent with delay 0 (`previews=0`), not a missing producer that fired on Source40 under the same delay.

Not `ENGINE_PREDICTION_NOT_EXECUTED`: no engine/source string `prediction_executed`; the delay-0 `local_prediction.previews=0` field is present and recorded.
