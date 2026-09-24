"""A client that loses its host never takes the match over alone: the split-brain arm of the service seam.

Runs the e2e scenario mp-host-silent (a two-peer match; the host goes silent for 4 s at tick 450, so the client loses it
while the host holds the client's seat) and judges from the run's files:

  1. the client never declares itself the host (`Host left - Client is now hosting` is the split brain);
  2. the client rejoins the host through its private rejoin (`private catch-up complete frame=E`) and is not landed;
  3. both peers play to tick 2400 and their tick hashes are equal on every tick the client simulated from its rejoin
     through the end (ruling D2: a peer's coverage starts at the image it loaded).

    python tools/test_host_silent.py --repo <tree> --out <dir> [--size 640x360] [--fps 2]
    python tools/test_host_silent.py --judge <dir>

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

CAP = 2400
SELF_HOSTED = re.compile(r"\[net-match\] Host left - (.+) is now hosting")
CAUGHT_UP = re.compile(r"\[net-match\] private catch-up complete frame=(\d+)")


def judge(out: Path, repo: Path) -> dict:
    sys.path.insert(0, str(repo / "tools"))
    from e2e_video import compare_hash_range  # noqa: PLC0415

    run = out / "run0"
    client = (run / "client" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    host = (run / "host" / "stdout.log").read_text(encoding="utf-8", errors="replace")
    failures = []
    verdict = {"client_took_over": SELF_HOSTED.findall(client), "catch_up_complete": CAUGHT_UP.findall(client),
               "host_stall": [line for line in host.splitlines() if "live stall" in line][:1],
               "client_landed": "The host left the match" in client}
    if verdict["client_took_over"]:
        failures.append(f"the client took the match over while its host lived: {verdict['client_took_over']}")
    if not verdict["catch_up_complete"]:
        failures.append("the client never completed a private rejoin to its host")
    if verdict["client_landed"]:
        failures.append("the client was sent to the landing")
    traces = {}
    for name in ("host", "client"):
        path = run / f"{name}_trace.json"
        traces[name] = {row["tick"] for row in json.loads(path.read_text(encoding="utf-8-sig"))["runs"][0]["tick_hashes"]} if path.is_file() else set()
    start = CAP
    while start - 1 > 450 and start - 1 in traces["client"]:
        start -= 1
    verdict["compared_from"] = start
    if start not in traces["client"] or any(tick not in traces["host"] for tick in range(start, CAP + 1)):
        failures.append(f"the peers do not both cover the ticks {start}-{CAP}")
    else:
        compared = compare_hash_range(run / "host_trace.json", run / "client_trace.json", start, CAP, out / "silent-hashes")
        verdict["hashes"] = {key: compared[key] for key in ("status", "first_tick", "last_tick", "first_difference", "first_missing_tick", "full_rows_equal")}
        if compared["status"] != "PASS":
            failures.append(f"the peers' hashes differ: first difference at {compared['first_difference']}")
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
                   "--scenario", "mp-host-silent", "--size", options.size, "--fps", str(options.fps)]
        subprocess.run(command, env=env, check=False)
    verdict = judge(out, options.repo)
    (out / "host-silent-verdict.json").write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(verdict, indent=2))
    print(f"[host-silent] {verdict['status']}")
    return 0 if verdict["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
