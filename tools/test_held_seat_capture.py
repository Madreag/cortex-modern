"""A seat held by the AI costs the survivors nothing: the held-seat A/B of the match service seam.

Runs the e2e scenario mp-held-seat (host, ClientA and ClientB; ClientB's process is dropped at tick 300 and its seat stays
under the AI for a minute while the host and ClientA play to tick 4300) and judges two things from the run's own files:

  1. the host takes no private rejoin base while nobody is returning: no `[checkpoint-capture] name=p5join_base_...` line
     in the host's log (a base is a sim-thread capture, and every peer waits on the host for it);
  2. the survivor never waits on the host longer than the slow-player bound: ClientA's match report,
     runner.lockstep.peers.1.longest_wait_ms <= 50.

    python tools/test_held_seat_capture.py --repo <tree> --out <dir> [--size 640x360]
    python tools/test_held_seat_capture.py --judge <dir>      # judge an existing capture only

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

BOUND_MS = 50
CAPTURE = re.compile(r"\[checkpoint-capture\] name=p5join_base_\S+ tick=(\d+) sim_block_ms=([0-9.]+)")


def judge(out: Path) -> dict:
    run = out / "run0"
    host_log = (run / "host" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    captures = [{"tick": int(tick), "sim_block_ms": float(ms)} for tick, ms in CAPTURE.findall(host_log)]
    report = json.loads((run / "clienta-match.json").read_text(encoding="utf-8"))
    peers = report.get("runner", {}).get("lockstep", {}).get("peers", {})
    host_wait = peers.get("1", {}).get("longest_wait_ms")
    held = [line for line in host_log.splitlines() if "AI in control" in line or " held " in line]
    host_ticks = report.get("runner", {}).get("lockstep", {}).get("completed_simulation_tick")
    verdict = {
        "captures": captures,
        "survivor_longest_wait_on_host_ms": host_wait,
        "bound_ms": BOUND_MS,
        "host_hold_lines": held[:5],
        "survivor_completed_tick": host_ticks,
    }
    failures = []
    if not held:
        failures.append("the host never held ClientB's seat")
    if captures:
        failures.append(f"the host took {len(captures)} private base capture(s) with nobody returning, the longest "
                        f"{max(c['sim_block_ms'] for c in captures):.1f} ms of sim-thread block")
    if host_wait is None or host_wait > BOUND_MS:
        failures.append(f"the survivor waited {host_wait} ms on the host, past the {BOUND_MS} ms bound")
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
                   "--scenario", "mp-held-seat", "--size", options.size, "--fps", str(options.fps)]
        subprocess.run(command, env=env, check=False)
    verdict = judge(out)
    (out / "held-seat-verdict.json").write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(verdict, indent=2))
    print(f"[held-seat-capture] {verdict['status']}")
    return 0 if verdict["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
