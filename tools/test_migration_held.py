"""A seat held when its host drops rejoins the new host through the private rejoin: the migration arm of the service seam.

Runs the e2e scenario mp-host-migration-held (host, ClientA, ClientB and ClientC; ClientB stalls at tick 450 for 4 s so the
host holds its seat, the host's process is dropped at tick 601, ClientA and ClientC elect ClientA) and judges from the run's files:

  1. ClientA and ClientC declare the same new host, boundary and round (`Host left - ClientA is now hosting; boundary=B round=R`),
     and ClientA and ClientC hash equal from the boundary through the end;
  2. ClientB's rejoin found its host gone and went to the successor, then completed its private catch-up
     (`held rejoin: the host is gone; rejoining the successor at ...`, `private catch-up complete frame=E`),
     never the round-stopping resync and never the landing;
  3. both survivors played to tick 5400 and their tick hashes are equal on every tick ClientB simulated after the
     boundary, through the end: ClientB's coverage starts at the image it loaded (ruling D2) and every tick from there is
     compared.

    python tools/test_migration_held.py --repo <tree> --out <dir> [--size 640x360]
    python tools/test_migration_held.py --judge <dir>

Every engine launch goes through tools/e2e_video.py and the runners with CCCP_HEADLESS=1.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

CAP = 5400
HOSTED = re.compile(r"(?m)^\[net-match\] Host left - (.+) is now hosting; boundary=(\d+) round=(\d+)")
SUCCESSOR = re.compile(r"\[net-match\] held rejoin: the host is gone; rejoining the successor at (\S+)")
CAUGHT_UP = re.compile(r"\[net-match\] private catch-up complete frame=(\d+)")


def compare_window(first: Path, second: Path, start: int, cap: int, out: Path) -> dict:
    """Both traces over [start, cap] only: a held peer's coverage starts at the image it loaded (ruling D2), so the window is
    validated and compared tick by tick and the held gap before it is not part of the comparison."""
    from compare_sim_traces import load_trace, strict_compare  # noqa: PLC0415

    out.mkdir(parents=True, exist_ok=True)
    rows, paths, errors = [], [], []
    for index, source in enumerate((first, second)):
        data = json.loads(Path(source).read_text(encoding="utf-8-sig"))
        data["runs"][0]["tick_hashes"] = [row for row in data["runs"][0]["tick_hashes"] if start <= row["tick"] <= cap]
        path = out / f"window-{index}.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        try:
            load_trace(path)
        except ValueError as error:
            errors.append(f"{source}: {error}")
        rows.append(data["runs"][0]["tick_hashes"])
        paths.append(path)
    passed, _ = strict_compare(*paths, expected_ticks=cap - start + 1, first_tick=start)
    coverage = all([row["tick"] for row in trace] == list(range(start, cap + 1)) for trace in rows)
    first_difference = next((a["tick"] for a, b in zip(*rows) if a != b), None)
    exact = coverage and rows[0] == rows[1]
    return {"status": "PASS" if passed and exact and not errors else "FAIL", "first_tick": start, "last_tick": cap,
            "first_difference": first_difference, "coverage": coverage, "compared_ticks": len(rows[0]), "validation_errors": errors}


def judge(out: Path, repo: Path) -> dict:
    sys.path.insert(0, str(repo / "tools"))

    run = out / "run0"
    logs = {name: (run / name / "stdout.log").read_text(encoding="utf-8", errors="replace") for name in ("clienta", "clientb", "clientc")}
    failures = []
    hosted = HOSTED.findall(logs["clienta"])
    if HOSTED.findall(logs["clientc"]) != hosted:
        failures.append(f"ClientC declared {HOSTED.findall(logs['clientc'])} where ClientA declared {hosted}")
    successor = SUCCESSOR.findall(logs["clientb"])
    caught_up = CAUGHT_UP.findall(logs["clientb"])
    landed = "The host left the match" in logs["clientb"]
    verdict = {"new_host": hosted, "successor_attempts": successor, "catch_up_complete": caught_up, "clientb_landed": landed}
    if len(hosted) != 1 or not hosted[0][0].startswith("ClientA"):
        failures.append(f"ClientA did not take the match over: {hosted}")
    if not successor:
        failures.append("ClientB never rejoined the successor")
    if not caught_up:
        failures.append("ClientB's private catch-up never completed")
    if landed:
        failures.append("ClientB was sent to the landing")
    # Every survivor ends clean: no protocol error on any peer, and each exits 0 (the host is the one dropped on purpose).
    for name, text in logs.items():
        errors = re.findall(r"ProtocolError:\S*", text)
        if errors:
            failures.append(f"{name} stopped on a protocol error: {errors[0][:240]}")
        launch = run / name / "launch.json"
        exit_code = json.loads(launch.read_text(encoding="utf-8-sig")).get("exit_code") if launch.is_file() else None
        if exit_code != 0:
            failures.append(f"{name} exited {exit_code}")
    verdict["protocol_errors"] = {name: len(re.findall(r"ProtocolError:", text)) for name, text in logs.items()}
    if hosted:
        boundary = int(hosted[0][1])
        # A survivor held later in the run is compared from the image it loaded, as every peer's coverage is (ruling D2).
        covered = {row["tick"] for row in json.loads((run / "clientc_trace.json").read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]}
        survivorStart = CAP
        while survivorStart - 1 > boundary and survivorStart - 1 in covered:
            survivorStart -= 1
        verdict["survivor_compared_from"] = survivorStart
        survivors = compare_window(run / "clienta_trace.json", run / "clientc_trace.json", survivorStart, CAP, out / "survivor-hashes")
        verdict["survivor_hashes"] = {key: survivors[key] for key in ("status", "first_tick", "last_tick", "first_difference", "coverage", "compared_ticks", "validation_errors")}
        if survivors["status"] != "PASS":
            failures.append(f"ClientA and ClientC differ after the boundary: {verdict['survivor_hashes']}")
        traces = {}
        for name in ("clienta", "clientb"):
            data = json.loads((run / f"{name}_trace.json").read_text(encoding="utf-8-sig"))
            traces[name] = {row["tick"] for row in data["runs"][0]["tick_hashes"]}
        # The first tick after the boundary from which ClientB simulated every tick through the cap.
        start = CAP
        while start - 1 > boundary and start - 1 in traces["clientb"]:
            start -= 1
        verdict["boundary"] = boundary
        verdict["compared_from"] = start
        if start not in traces["clientb"] or any(tick not in traces["clienta"] for tick in range(start, CAP + 1)):
            failures.append(f"the survivors do not both cover the ticks {start}-{CAP}")
        else:
            compared = compare_window(run / "clienta_trace.json", run / "clientb_trace.json", start, CAP, out / "held-hashes")
            verdict["hashes"] = {key: compared[key] for key in ("status", "first_tick", "last_tick", "first_difference", "coverage", "compared_ticks", "validation_errors")}
            if compared["status"] != "PASS":
                failures.append(f"the survivors' hashes differ: first difference at {compared['first_difference']}")
    verdict["failures"] = failures
    verdict["status"] = "PASS" if not failures else "FAIL"
    return verdict


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--fps", type=int, default=2, help="frames per second the capture keeps; the judge reads logs and traces, not frames")
    parser.add_argument("--judge", type=Path, help="judge an existing capture directory without launching")
    options = parser.parse_args()
    out = options.judge or options.out
    if out is None:
        parser.error("--out or --judge is required")
    if not options.judge:
        env = dict(os.environ, CCCP_HEADLESS="1")
        command = [sys.executable, str(options.repo / "tools" / "e2e_video.py"), "--repo", str(options.repo), "--out", str(out),
                   "--scenario", "mp-host-migration-held", "--size", options.size, "--fps", str(options.fps)]
        subprocess.run(command, env=env, check=False)
    verdict = judge(out, options.repo)
    (out / "migration-held-verdict.json").write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(verdict, indent=2))
    print(f"[migration-held] {verdict['status']}")
    return 0 if verdict["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
