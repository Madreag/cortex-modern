"""Resume two retained runtimes and compare every tick after their checkpoint.

The host relaunches its own retained runtime through the real flags a player's resume uses
(``-net-resume-match``/``-net-resume-tick``) and the client joins it fresh, so the run exercises
the admission a restored match offers rather than a pair of freshly configured peers. Both peers
run in place: a retained runtime under a run root is never copied or moved.

    python tools/feel/retained_resume.py --repo <tree> --out <dir> \
        --runtime-host <run>/host/runtime --runtime-client <run>/client/runtime \
        --match <matchId> --tick <savedTick> --port 49470

The port is the host's listen port and must sit in the caller's reserved private block
(this lane's block is 49470-49499); a port outside the private range is refused.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import threading
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run

# The private port range: a lane takes its own block inside it so two lanes never share a listener.
PRIVATE_PORTS = (49152, 65535)
# A resumed peer that is resynced replays the ticks it already ran, so a pass is compared on its own.
MIN_SHARED_TICKS = 30


def read_live_hashes(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def split_passes(lines: list[dict]) -> list[list[dict]]:
    passes: list[list[dict]] = []
    for entry in lines:
        if not passes or entry["tick"] <= passes[-1][-1]["tick"]:
            passes.append([])
        passes[-1].append(entry)
    return passes


def compare_live_hashes(host_path: Path, client_path: Path, first_tick: int) -> list[dict]:
    """Every tick both peers ran carries the same gated hash and the same subsystem hashes."""
    host = {entry["tick"]: entry for entry in read_live_hashes(host_path)}
    results = []
    for index, run in enumerate(split_passes(read_live_hashes(client_path))):
        shared = [entry for entry in run if entry["tick"] >= first_tick and entry["tick"] in host]
        mismatched = [entry["tick"] for entry in shared
                      if entry["sim_gated"] != host[entry["tick"]]["sim_gated"] or entry["subsystems"] != host[entry["tick"]]["subsystems"]]
        subsystems = sorted({name for entry in shared for name, value in entry["subsystems"].items()
                             if value != host[entry["tick"]]["subsystems"].get(name)})
        results.append({"pass": index, "first_tick": run[0]["tick"], "last_tick": run[-1]["tick"], "compared_ticks": len(shared),
                        "mismatched_ticks": len(mismatched), "first_mismatches": mismatched[:8], "subsystems": subsystems})
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--runtime-host", type=Path, required=True)
    parser.add_argument("--runtime-client", type=Path, required=True)
    parser.add_argument("--ticket-host", type=Path)
    parser.add_argument("--ticket-client", type=Path)
    parser.add_argument("--input-script-host", type=Path)
    parser.add_argument("--input-script-client", type=Path)
    parser.add_argument("--match", required=True)
    parser.add_argument("--tick", type=int, required=True)
    parser.add_argument("--ticks", type=int, default=1200)
    parser.add_argument("--port", type=int, required=True, help=f"host listen port, inside {PRIVATE_PORTS[0]}-{PRIVATE_PORTS[1]}")
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    if options.tick < 1 or options.ticks < 30:
        parser.error("tick must be positive and ticks at least 30")
    if not PRIVATE_PORTS[0] <= options.port <= PRIVATE_PORTS[1]:
        parser.error(f"--port must be in the private range {PRIVATE_PORTS[0]}-{PRIVATE_PORTS[1]}")
    os.environ["CCCP_HEADLESS"] = "1"
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    archive = options.runtime_host / "Autosaves" / f"{options.match}-{options.tick}.ccsave"
    archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    with zipfile.ZipFile(archive) as checkpoint:
        restore = next(name for name in checkpoint.namelist() if Path(name).name == "Restore.ini")
        descriptor = dict(line.strip().split(" = ", 1) for line in checkpoint.read(restore).decode("utf-8").splitlines() if " = " in line)
    expected_hash = descriptor.get("DeterministicConfigHash", "")
    if not expected_hash:
        raise ValueError("the retained checkpoint has no deterministic config hash")

    runs, records, reports, identities = {}, {}, {}, {}
    for who in ("host", "client"):
        args = ["-net-match-service-e2e", "-net-port", str(options.port), "-net-match-peers", "2",
                "-net-match-ticks", str(options.ticks), "-net-match-input-delay", "3", "-net-autosave-seconds", "1",
                "-net-match-e2e-resync", "-tick-hashes", "-out", str(root / f"{who}_trace.json"),
                "-net-match-report", str(root / f"{who}-match.json"),
                "-net-live-tick-hashes", str(root / f"{who}-live.jsonl")]
        ticket, script = getattr(options, f"ticket_{who}"), getattr(options, f"input_script_{who}")
        if ticket:
            args += ["-net-reconnect-ticket", str(ticket.resolve())]
        if script:
            args += ["-input-script", str(script.resolve())]
        args += (["-net-host", "-net-resume-match", options.match, "-net-resume-tick", str(options.tick)]
                 if who == "host" else ["-net-join", "127.0.0.1"])
        runs[who] = make_run(options.repo, args, root / who, options.timeout,
                             env={"CCCP_HEADLESS": "1", "CC_KEEP_RESYNC_SAVES": "1"},
                             runtime=getattr(options, f"runtime_{who}"))

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}
        finally:
            runs[who].close()

    threads = [threading.Thread(target=drive, args=(who,)) for who in ("host", "client")]
    try:
        threads[0].start()
        time.sleep(2)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    failures = []
    for who in ("host", "client"):
        record = records.get(who, {})
        if record.get("exit_code") != 0 or record.get("timed_out"):
            failures.append(f"{who}: runner {record.get('exit_code')} {record.get('error', '')}")
        path = root / f"{who}-match.json"
        report = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        reports[who] = report
        if report.get("exit_code") != 0 or report.get("setup_error") or report.get("runtime_error"):
            failures.append(f"{who}: {report.get('setup_error')} {report.get('runtime_error')}")
        if report.get("running_ticks", 0) < options.ticks:
            failures.append(f"{who}: did not complete its tick cap")
        runner = report.get("service", {}).get("runner", {})
        identity = runner.get("session", {}).get("local_identity", {})
        identities[who] = identity.get("deterministic_config_hash", "")
        if identities[who] != expected_hash:
            failures.append(f"{who}: admission differs from the checkpoint's config hash: checkpoint={expected_hash} {who}={identities[who]}")
        if runner.get("lockstep", {}).get("host_peer_id") != 1:
            failures.append(f"{who}: changed authority during resume")
        check = report.get("desync_check", {})
        if check.get("compares", 0) == 0 or check.get("mismatches", 0) != 0:
            failures.append(f"{who}: missing or mismatched live checksum comparisons")
    comparison = compare_live_hashes(root / "host-live.jsonl", root / "client-live.jsonl", options.tick + 1)
    if not comparison:
        failures.append("the client recorded no live tick hashes")
    for run in comparison:
        if run["compared_ticks"] < MIN_SHARED_TICKS:
            failures.append(f"pass {run['pass']} shared only {run['compared_ticks']} ticks with the host")
        if run["mismatched_ticks"]:
            failures.append(f"pass {run['pass']} ({run['first_tick']}-{run['last_tick']}) differs from the host on "
                            f"{run['mismatched_ticks']} of {run['compared_ticks']} ticks: {run['first_mismatches']} {run['subsystems']}")
    passed = not failures
    compared_ticks = sum(run["compared_ticks"] for run in comparison)
    line = f"[retained-resume] {'PASS' if passed else 'FAIL'} tick={options.tick} passes={len(comparison)} compared_ticks={compared_ticks}"
    result = {"passed": passed, "final_line": line, "failures": failures, "comparison": comparison,
              "checkpoint_sha256": archive_hash, "deterministic_config_hash": expected_hash,
              "peer_config_hashes": identities, "port": options.port,
              "desync_checks": {who: reports[who].get("desync_check", {}) for who in ("host", "client")},
              "retained_runtimes": {who: str(getattr(options, f"runtime_{who}").resolve()) for who in ("host", "client")}}
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(line)
    for failure in failures:
        print(failure)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
