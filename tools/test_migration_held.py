"""A seat held when its host drops rejoins the new host through the private rejoin: the migration arm of the service seam.

Runs the e2e scenario mp-host-migration-held (host, ClientA and ClientB; ClientB stalls at tick 450 for 6 s so the host
holds its seat, the host's process is dropped at tick 601, ClientA takes the match over) and judges from the run's files:

  1. ClientA declares itself the new host (`Host left - ClientA is now hosting; boundary=B round=R`);
  2. ClientB's rejoin found its host gone and went to the successor, then completed its private catch-up
     (`held rejoin: the host is gone; rejoining the successor at ...`, `private catch-up complete frame=E`),
     never the round-stopping resync and never the landing;
  3. both survivors played to tick 900 and their tick hashes are equal on every tick ClientB simulated after the
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

CAP = 900
HOSTED = re.compile(r"(?m)^\[net-match\] Host left - (.+) is now hosting; boundary=(\d+) round=(\d+)")
SUCCESSOR = re.compile(r"\[net-match\] held rejoin: the host is gone; rejoining the successor at (\S+)")
CAUGHT_UP = re.compile(r"\[net-match\] private catch-up complete frame=(\d+)")


def judge(out: Path, repo: Path) -> dict:
    sys.path.insert(0, str(repo / "tools"))
    from e2e_video import compare_hash_range  # noqa: PLC0415

    run = out / "run0"
    logs = {name: (run / name / "stdout.log").read_text(encoding="utf-8", errors="replace") for name in ("clienta", "clientb")}
    failures = []
    hosted = HOSTED.findall(logs["clienta"])
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
    if hosted:
        boundary = int(hosted[0][1])
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
            compared = compare_hash_range(run / "clienta_trace.json", run / "clientb_trace.json", start, CAP, out / "held-hashes")
            verdict["hashes"] = {key: compared[key] for key in ("status", "first_tick", "last_tick", "first_difference", "first_missing_tick", "full_rows_equal")}
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
    parser.add_argument("--judge", type=Path, help="judge an existing capture directory without launching")
    options = parser.parse_args()
    out = options.judge or options.out
    if out is None:
        parser.error("--out or --judge is required")
    if not options.judge:
        env = dict(os.environ, CCCP_HEADLESS="1")
        command = [sys.executable, str(options.repo / "tools" / "e2e_video.py"), "--repo", str(options.repo), "--out", str(out),
                   "--scenario", "mp-host-migration-held", "--size", options.size]
        subprocess.run(command, env=env, check=False)
    verdict = judge(out, options.repo)
    (out / "migration-held-verdict.json").write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(verdict, indent=2))
    print(f"[migration-held] {verdict['status']}")
    return 0 if verdict["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
