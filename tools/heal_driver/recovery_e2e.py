"""Fresh lockstep cases with validated traces and a private runtime per process."""

from pathlib import Path
import datetime
import hashlib
import json
import os
import subprocess
import sys
import threading
import uuid
import time

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
from compare_sim_traces import strict_compare
from launched_exe import apply_launched_exe
from run_sim_test import make_run

ROOT = Path('D:/Projects/stage2_p4/recovery_runs') / (datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d_%H%M%S_') + uuid.uuid4().hex[:8])
EXE = REPO / 'Cortex Command.exe'
OUT = ROOT / 'fresh' / 'e2e'
SNAPSHOT_COMPARE = REPO / "tools" / "compare_snapshots.py"


def sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def run_isolated(argv, out_dir, timeout, expected=None):
    return make_run(REPO, argv[1:], out_dir, timeout, expected=expected)


def verify_recording(replay: Path, out: Path):
    vrec = (
        run_isolated([str(EXE), "-net-replay-verify", str(replay)], out, 60)
        .start()
        .finish()
    )
    vlog = (out / "stdout.log").read_text(encoding="utf-8-sig", errors="replace")
    line = next((l for l in vlog.splitlines() if "[net-replay-verify]" in l), "")
    try:
        data = json.loads(line.split("]", 1)[1]) if line else {}
    except ValueError:
        data = {}
    return vrec, data


# Every lane: name, port, input delay, host/client extra args, ticks, traced?, mode, notes.
LANES = {
    "d0_baseline": dict(
        port=43300,
        delay=0,
        mode="normal",
        what="baseline duel, D=0: the canonical two-peer PvP match (regression-only: no previews at D=0)",
    ),
    "d3_prediction": dict(
        port=43310,
        delay=3,
        mode="normal",
        what="baseline duel at input delay 3 with local prediction previewing the host's actor",
    ),
    "d3_buy_order": dict(
        port=43320,
        delay=3,
        host=["-net-match-e2e-buy-command"],
        mode="normal",
        ticks=900,
        what="host grants 5000 funds at tick 50 and places a real buy order at 80; the craft arrives ~351 ticks later; funds probe at 700",
    ),
    "d3_deliver_cargo": dict(
        port=43330,
        delay=3,
        host=["-net-match-e2e-deliver-command"],
        mode="normal",
        what="host delivers a craft with cargo by command at tick 50",
    ),
    "d3_brain_kill": dict(
        port=43340,
        delay=3,
        host=["-net-match-e2e-brain-kill-command"],
        mode="normal",
        what="host delivers + scuttles a craft onto the enemy brain; the win condition must end the match identically",
    ),
    "d3_funds": dict(
        port=43350,
        delay=3,
        host=["-net-match-e2e-funds-command"],
        mode="normal",
        what="host issues a synced SetTeamFunds command at tick 50",
    ),
    "d3_spawn": dict(
        port=43360,
        delay=3,
        host=["-net-match-e2e-spawn-command"],
        mode="normal",
        what="host spawns an actor by command at tick 50; both peers must clone it identically",
    ),
    "d3_scuttle": dict(
        port=43370,
        delay=3,
        host=["-net-match-e2e-scuttle-command", "-net-match-e2e-deliver-command"],
        mode="normal",
        what="host scuttles the tick-50 delivered craft at tick 100",
    ),
    "d3_inventory": dict(
        port=43380,
        delay=3,
        host=["-net-match-e2e-inventory-command"],
        mode="normal",
        what="host-issued inventory ops (reorder/swap/reload/drop) at ticks 210-300",
    ),
    "d3_ai_orders": dict(
        port=43390,
        delay=3,
        host=["-net-match-e2e-ai-order-command"],
        mode="normal",
        ticks=900,
        what="host sends go-to / follow / squad / disband AI orders at 50/200/400/600",
    ),
    "d3_pause": dict(
        port=43400,
        delay=3,
        host=["-net-match-e2e-pause-command"],
        mode="normal",
        ticks=900,
        what="host pauses at 250 and unpauses at 430 (synced pause: sim time frozen, null ticks exchanged)",
    ),
    "d3_stall": dict(
        port=43410,
        delay=3,
        client=["-net-match-e2e-stall"],
        mode="normal",
        timeout=400,
        what="client fakes an 8 s frame stall at tick 300; the host must ride it out within the 20 s grace",
    ),
    "d3_stall_repeat": dict(
        port=43415,
        delay=3,
        client=["-net-match-e2e-stall"],
        mode="normal",
        timeout=400,
        what="repeat of d3_stall to see whether the client's post-cap failure reproduces",
    ),
    "fakelag100_auto": dict(
        port=43420,
        delay=0,
        host=["-net-fake-lag", "100", "-net-match-auto-delay"],
        client=["-net-fake-lag", "100"],
        mode="normal",
        expect_prediction=True,
        timeout=400,
        what="both peers add 100 ms fake lag; the host auto-picks the input delay from the measured RTT",
    ),
    "perturb_control": dict(
        port=43430,
        delay=0,
        host=["-determinism-selftest-perturb"],
        mode="perturb",
        what="POSITIVE CONTROL: the host's sim is perturbed at tick 50; the divergence MUST be caught",
    ),
    "mismatch_control": dict(
        port=43440,
        delay=0,
        host=["-num-lua-states", "4", "-net-match-e2e-join-rejection"],
        client=["-num-lua-states", "8"],
        mode="mismatch",
        traced=False,
        timeout=120,
        what="POSITIVE CONTROL: a deliberate identity mismatch (nls 4 vs 8) MUST be rejected with the reason on both peers",
    ),
    "brain_spawn": dict(
        port=43450,
        delay=3,
        host=["-net-match-e2e-brain-kill-command"],
        client=["-net-match-e2e-brain-spawn-command"],
        mode="brainspawn",
        what="the client spawns a second brain for its team, then the host kills the original; the match must NOT end",
    ),
    "snapshot_p5": dict(
        port=43460,
        delay=3,
        host=["-net-match-e2e-snapshot"],
        client=["-net-match-e2e-snapshot"],
        mode="snapshot",
        what="both peers save the full game at synced tick 300; the sim payloads must be byte-identical",
    ),
    "resync_heal": dict(
        port=47661,
        delay=0,
        host=["-determinism-selftest-perturb", "-net-match-e2e-resync"],
        client=["-net-match-e2e-resync"],
        mode="resync",
        traced=False,
        timeout=400,
        what="the host perturbs its sim at 50; runtime desync detection must fire and both peers must heal from the host's snapshot",
    ),
    "rematch": dict(
        port=43480,
        delay=0,
        host=["-net-match-e2e-rematch"],
        client=["-net-match-e2e-rematch"],
        mode="rematch",
        traced=False,
        timeout=400,
        what="match 1 -> lobby -> match 2 to the cap with the live desync exchange clean",
    ),
}


def match_frame_liveness(rep):
    # completed_simulation_tick is racy by one at e2e stop; the coordinator commit is next_frame.
    lockstep = ((rep.get("service") or {}).get("runner") or {}).get("lockstep") or {}
    next_frame = lockstep.get("next_frame") or 0
    planned = rep.get("frames_planned") or 0
    delay = lockstep.get("input_delay_frames")
    if delay is None:
        delay = lockstep.get("input_delay")
    ok = next_frame > planned and not rep.get("runtime_error") and not rep.get("setup_error")
    return ok, next_frame, planned, delay


def lane(name: str, spec: dict):
    out = OUT / name
    out.mkdir(parents=True, exist_ok=False)
    ticks = spec.get("ticks", 600)
    traced = spec.get("traced", True)
    mode = spec.get("mode", "normal")
    host_report, client_report = out / "host_report.json", out / "client_report.json"
    host_trace, client_trace = out / "host_trace.json", out / "client_trace.json"
    replay = out / "match.ccreplay"
    common = [
        "-net-match-service-e2e",
        "-net-port",
        str(spec["port"]),
        "-net-match-ticks",
        str(ticks),
    ]
    if traced:
        common += ["-tick-hashes", "-max-ticks", str(ticks)]
    host_argv = [str(EXE), *common, "-net-host", "-net-match-report", str(host_report)]
    client_argv = [
        str(EXE),
        *common,
        "-net-join",
        "127.0.0.1",
        "-net-match-report",
        str(client_report),
    ]
    if traced:
        host_argv += ["-out", str(host_trace), "-net-replay-out", str(replay)]
        client_argv += ["-out", str(client_trace)]
    if spec.get("delay", 0) > 0:
        host_argv += ["-net-match-input-delay", str(spec["delay"])]
        client_argv += ["-net-match-input-delay", str(spec["delay"])]
    host_argv += spec.get("host", [])
    client_argv += spec.get("client", [])
    timeout = spec.get("timeout", 300)
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    host = run_isolated(
        host_argv,
        out / "host",
        timeout,
        [host_report] + ([host_trace] if traced else []),
    )
    client = run_isolated(
        client_argv,
        out / "client",
        timeout,
        [client_report] + ([client_trace] if traced else []),
    )
    records = {}

    def drive(run, key):
        try:
            run.start()
            records[key] = run.finish()
        except Exception as exc:
            records[key] = {"error": repr(exc)}

    host_thread = threading.Thread(target=drive, args=(host, "host"))
    host_thread.start()
    time.sleep(1.5)
    client_thread = threading.Thread(target=drive, args=(client, "client"))
    client_thread.start()
    host_thread.join()
    client_thread.join()

    checks = []

    def check(name_, ok, detail, evidence):
        checks.append(
            {
                "name": name_,
                "status": "pass" if ok else "fail",
                "detail": detail,
                "evidence": [
                    str(Path(e).relative_to(ROOT)) if Path(e).is_absolute() else str(e)
                    for e in evidence
                ],
            }
        )

    reports = {}
    for key, path in (("host", host_report), ("client", client_report)):
        if path.exists():
            try:
                reports[key] = json.loads(path.read_text(encoding="utf-8-sig"))
            except ValueError as exc:
                reports[key] = {"parse_error": str(exc)}
    exits = {k: records.get(k, {}).get("exit_code") for k in ("host", "client")}

    if mode == "mismatch":
        check("host_kept_listening", reports.get("host", {}).get("service", {}).get("state") == "Starting", "a rejected join leaves the host in the lobby", [host_report])
        for key in ("host", "client"):
            record = records.get(key, {})
            check(key + "_clean_rejection_exit", record.get("exit_code") == 1 and not record.get("timed_out"), f"exit={record.get('exit_code')} timeout={record.get('timed_out')}", [out / key / "launch.json"])
            rep = reports.get(key, {})
            setup_error = str(rep.get("setup_error", ""))
            service_error = str(rep.get("service", {}).get("error", ""))
            rich = any(
                w in (setup_error + service_error).lower()
                for w in ("mismatch", "does not match", "lua", "identity", "reject")
            )
            check(
                f"{key}_rejected_with_reason",
                bool(setup_error) and bool(service_error) and rich,
                f"exit={exits[key]} setup_error={setup_error[:160]!r} service.error={service_error[:160]!r}",
                [out / f"{key}_report.json", out / key / "stdout.log"],
            )
        check(
            "no_match_ran",
            all(reports.get(k, {}).get("running_ticks", 0) == 0 for k in reports)
            and len(reports) == 2,
            f"running_ticks host={reports.get('host', {}).get('running_ticks')} client={reports.get('client', {}).get('running_ticks')}",
            [host_report, client_report],
        )
    elif mode == "perturb":
        errs = " | ".join(
            str(reports.get(k, {}).get("runtime_error", "")) for k in reports
        )
        caught_runtime = any(
            w in errs.lower() for w in ("apply failed", "desync", "diverged")
        )
        caught_compare = False
        detail_compare = None
        if host_trace.exists() and client_trace.exists():
            ok, detail_compare = strict_compare(host_trace, client_trace, ticks)
            (out / "strict_compare.json").write_text(
                json.dumps(detail_compare, indent=2), encoding="utf-8"
            )
            caught_compare = detail_compare["first_divergence"] is not None
        check(
            "positive_control_divergence_caught",
            caught_runtime or caught_compare,
            f"runtime_errors={errs[:200]!r} compare_first_divergence={detail_compare['first_divergence'] if detail_compare else None} subsystems={detail_compare['divergent_subsystems'] if detail_compare else None}",
            [host_report, client_report, out / "strict_compare.json"],
        )
        check(
            "processes_finished",
            all(v is not None for v in exits.values()),
            f"exit host={exits['host']} client={exits['client']} (a divergence may end the match early: non-zero is acceptable here)",
            [out / "host" / "launch.json", out / "client" / "launch.json"],
        )
    else:
        for key in ("host", "client"):
            rec = records.get(key, {})
            check(
                f"{key}_process_exit",
                rec.get("exit_code") == 0 and not rec.get("timed_out"),
                f"exit_code={rec.get('exit_code')} timed_out={rec.get('timed_out')} elapsed={rec.get('elapsed_seconds')}",
                [out / key / "launch.json", out / key / "stdout.log"],
            )
            check(
                f"{key}_report_present",
                key in reports and "parse_error" not in reports[key],
                str((out / f"{key}_report.json").name),
                [out / f"{key}_report.json"],
            )
        for key, rep in reports.items():
            service = rep.get("service", {})
            runner = service.get("runner", {})
            lockstep = runner.get("lockstep", {})
            reason = str(lockstep.get("timeout_reason", ""))
            alive, next_frame, planned, delay = match_frame_liveness(rep)
            delay_part = f" input_delay={delay}" if delay is not None else ""
            check(
                f"{key}_liveness",
                alive,
                f"next_frame={next_frame} frames_planned={planned}{delay_part} completed_simulation_tick={lockstep.get('completed_simulation_tick')} running_ticks={rep.get('running_ticks')} runtime_error={rep.get('runtime_error')!r} setup_error={rep.get('setup_error')!r} activity_state={rep.get('activity_state')} winner_team={rep.get('winner_team')}",
                [out / f"{key}_report.json"],
            )
            check(
                f"{key}_lockstep_completed",
                service.get("state") == "Running"
                and runner.get("state") == "Running"
                and reason.startswith("Complete:"),
                f"service={service.get('state')} runner={runner.get('state')} timeout_reason={reason!r} input_delay={lockstep.get('input_delay')}",
                [out / f"{key}_report.json"],
            )
            check(
                f"{key}_no_editor",
                not rep.get("entered_editor"),
                f"entered_editor={rep.get('entered_editor')}",
                [out / f"{key}_report.json"],
            )
            pace = rep.get("pace", {})
            check(
                f"{key}_pace_recorded",
                "wall_tps" in pace,
                f"wall_tps={pace.get('wall_tps')} sim_ms_per_tick={pace.get('sim_ms_per_tick')} (recorded, not asserted: two peers share one CPU here)",
                [out / f"{key}_report.json"],
            )
            if mode == "rematch":
                check(
                    f"{key}_rematch_round2",
                    rep.get("rematches") == 1 and rep.get("winner_team") == 0,
                    f"rematches={rep.get('rematches')} winner_team={rep.get('winner_team')} (round 2 must reach the cap with team 0 winning, as the harness pins)",
                    [out / f"{key}_report.json"],
                )
            if mode == "resync":
                check(
                    f"{key}_resynced",
                    rep.get("resyncs", 0) >= 1
                    and 1 <= rep.get("actors", 0) <= spec.get("initial_actors", 4)
                    and rep.get("actors_peak", 0) <= spec.get("max_actors", spec.get("initial_actors", 4)),
                    f"resyncs={rep.get('resyncs')} actors={rep.get('actors')} actors_peak={rep.get('actors_peak')} initial_actors={spec.get('initial_actors', 4)}",
                    [out / f"{key}_report.json"],
                )
            if mode == "brainspawn":
                check(
                    f"{key}_match_not_ended_by_brain_kill",
                    rep.get("winner_team") == -1,
                    f"winner_team={rep.get('winner_team')} (the second brain keeps the team alive)",
                    [out / f"{key}_report.json"],
                )
        if len(reports) == 2:
            lobby_h = (
                reports["host"].get("service", {}).get("runner", {}).get("lobby", {})
            )
            lobby_c = (
                reports["client"].get("service", {}).get("runner", {}).get("lobby", {})
            )
            hh = reports["host"].get("match_config_hash") or lobby_h.get(
                "match_config_hash"
            )
            ch = reports["client"].get("match_config_hash") or lobby_c.get(
                "match_config_hash"
            )
            check(
                "config_agreement",
                bool(hh) and hh == ch,
                f"host={hh} client={ch}",
                [host_report, client_report],
            )
            ha, ca = reports["host"].get("actors"), reports["client"].get("actors")
            bound = 4 if not (spec.get("host") or spec.get("client")) else 8
            check(
                "census_agreement",
                ha is not None and ha == ca and 1 <= ha <= bound,
                f"host actors={ha} client actors={ca} (peak host={reports['host'].get('actors_peak')} client={reports['client'].get('actors_peak')}); a matching count says the rosters agree, not what the actors did",
                [host_report, client_report],
            )
            cfg = lobby_h.get("match_config", {})
            check(
                "scenario_identity_recorded",
                bool(cfg.get("activity_preset")) and bool(cfg.get("scene_name")),
                f"activity={cfg.get('activity_preset')} scene={cfg.get('scene_name')} mode={cfg.get('mode')} input_delay={cfg.get('input_delay_frames')} peers={cfg.get('peer_count')} players={cfg.get('players')}",
                [host_report],
            )
            lp = reports["host"].get("local_prediction", {})
            check(
                "prediction_counters_recorded",
                "previews" in lp,
                f"enabled={lp.get('enabled')} previews={lp.get('previews')} actor_ticks={lp.get('actor_ticks')} taken={lp.get('taken')} violations={lp.get('violations')}",
                [host_report],
            )
            if spec.get("delay", 0) > 0 or spec.get("expect_prediction"):
                check(
                    "prediction_executed",
                    bool(lp.get("enabled"))
                    and lp.get("previews", 0) > 0
                    and lp.get("violations", 1) == 0,
                    f"previews={lp.get('previews')} violations={lp.get('violations')}",
                    [host_report],
                )
        if traced:
            if host_trace.exists() and client_trace.exists():
                ok, detail = strict_compare(host_trace, client_trace, ticks)
                (out / "strict_compare.json").write_text(
                    json.dumps(detail, indent=2), encoding="utf-8"
                )
                check(
                    "simgated_strict",
                    ok,
                    f"compared_ticks={detail['compared_ticks']} first_divergence={detail['first_divergence']} reasons={detail['reasons'][:3]} subsystems={detail.get('subsystem_sets_seen')}",
                    [out / "strict_compare.json", host_trace, client_trace],
                )
            else:
                check(
                    "simgated_strict",
                    False,
                    "a trace is missing",
                    [host_trace, client_trace],
                )
            rr = reports.get("host", {}).get("replay_recording", {})
            check(
                "replay_recording_closed",
                replay.exists()
                and rr.get("closed") is True
                and rr.get("frames", 0) >= ticks - 5,
                f"exists={replay.exists()} frames={rr.get('frames')} closed={rr.get('closed')} bytes={replay.stat().st_size if replay.exists() else 0}",
                [replay, host_report],
            )
            if replay.exists():
                vrec, vjson = verify_recording(replay, out / "replay_verify")
                check(
                    "replay_verify",
                    vrec.get("exit_code") == 0
                    and (vjson.get("outcome") == "ok" or vjson.get("ok") is True),
                    f"exit={vrec.get('exit_code')} verify={json.dumps(vjson)[:300]}",
                    [out / "replay_verify" / "stdout.log"],
                )
                play_out = out / "replay_playback"
                play_trace = play_out / "playback_trace.json"
                prec = (
                    run_isolated(
                        [
                            str(EXE),
                            "-net-replay",
                            str(replay),
                            "-tick-hashes",
                            "-max-ticks",
                            str(ticks),
                            "-out",
                            str(play_trace),
                        ],
                        play_out,
                        timeout,
                        [play_trace],
                    )
                    .start()
                    .finish()
                )
                plog = (play_out / "stdout.log").read_text(
                    encoding="utf-8-sig", errors="replace"
                )
                outcome_line = next(
                    (l for l in plog.splitlines() if "[net-replay] playback" in l), ""
                )
                check(
                    "replay_playback_completed",
                    prec.get("exit_code") == 0
                    and (
                        "outcome=completed" in outcome_line
                        or "outcome=tick_cap" in outcome_line
                    ),
                    f"exit={prec.get('exit_code')} {outcome_line.strip()}",
                    [play_out / "stdout.log"],
                )
                if play_trace.exists() and host_trace.exists():
                    ok, detail = strict_compare(host_trace, play_trace, ticks)
                    (out / "playback_compare.json").write_text(
                        json.dumps(detail, indent=2), encoding="utf-8"
                    )
                    check(
                        "replay_playback_matches_host",
                        ok,
                        f"compared_ticks={detail['compared_ticks']} first_divergence={detail['first_divergence']} reasons={detail['reasons'][:3]}",
                        [out / "playback_compare.json"],
                    )
                else:
                    check(
                        "replay_playback_matches_host",
                        False,
                        "playback wrote no trace",
                        [play_out / "stdout.log"],
                    )
        if mode == "snapshot":
            saves = sorted(save for peer in ("host", "client") for save in (out / peer / "runtime/Userdata/UserSavedGames.rte").glob("p5snap_p*.ccsave"))
            copied = []
            for save in saves:
                dest = out / save.name
                dest.write_bytes(save.read_bytes())
                copied.append(dest)
            check(
                "snapshots_written",
                len(copied) == 2,
                f"saves={[p.name for p in copied]}",
                [p for p in copied],
            )
            if len(copied) == 2:
                if not SNAPSHOT_COMPARE.exists():
                    missing = out / "snapshot_compare.txt"
                    missing.write_text(f"comparer missing: {SNAPSHOT_COMPARE}\n", encoding="utf-8")
                    check(
                        "snapshots_sim_identical",
                        False,
                        f"comparer missing: {SNAPSHOT_COMPARE}",
                        [missing],
                    )
                else:
                    proc = subprocess.run(
                        [
                            sys.executable,
                            str(SNAPSHOT_COMPARE),
                            str(copied[0]),
                            str(copied[1]),
                            "--peer-report-a",
                            str(host_report),
                            "--peer-report-b",
                            str(client_report),
                            "--cross-process",
                        ],
                        capture_output=True,
                        text=True,
                        env={**os.environ, "CCCP_TOOLS_DIR": str(REPO / "tools")},
                    )
                    (out / "snapshot_compare.txt").write_text(
                        proc.stdout + proc.stderr, encoding="utf-8"
                    )
                    check(
                        "snapshots_sim_identical",
                        proc.returncode == 0,
                        (proc.stdout + proc.stderr).strip()[-300:],
                        [out / "snapshot_compare.txt"],
                    )

    result = {
        "lane": name,
        "what": spec.get("what"),
        "mode": mode,
        "evidence_kind": "fresh-recovery",
        "started_utc": started,
        "port": spec["port"],
        "input_delay": spec.get("delay", 0),
        "ticks": ticks,
        "traced": traced,
        "argv": {"host": host_argv, "client": client_argv},
        "launch": records,
        "checks": checks,
        "pass": all(c["status"] == "pass" for c in checks),
    }
    apply_launched_exe(result, [out / "host" / "launch.json", out / "client" / "launch.json"])
    (out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(
        name,
        "PASS" if result["pass"] else "FAIL",
        [c["name"] for c in checks if c["status"] != "pass"],
        flush=True,
    )
    return result


if __name__ == "__main__":
    print("Evidence:", ROOT, flush=True)
    which = sys.argv[1:] or ["d0_baseline", "d3_prediction"]
    if which == ["all"]:
        which = list(LANES)
    lanes = [lane(name, LANES[name]) for name in which]
    summary_path = OUT / "result.json"
    previous = (
        json.loads(summary_path.read_text(encoding="utf-8"))
        if summary_path.exists()
        else {"lanes": []}
    )
    merged = {l["lane"]: l for l in previous.get("lanes", [])}
    for l in lanes:
        merged[l["lane"]] = {
            "lane": l["lane"],
            "what": l["what"],
            "mode": l["mode"],
            "pass": l["pass"],
            "failed_checks": [c["name"] for c in l["checks"] if c["status"] != "pass"],
            "result": f"fresh/e2e/{l['lane']}/result.json",
        }
    summary = {
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "evidence_kind": "fresh-recovery",
        "exe_sha256": sha256(EXE),
        "lanes": list(merged.values()),
    }
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(
        json.dumps(
            [{k: v for k, v in l.items() if k != "result"} for l in summary["lanes"]],
            indent=2,
        )
    )

    raise SystemExit(0 if all(l["pass"] for l in lanes) else 1)
