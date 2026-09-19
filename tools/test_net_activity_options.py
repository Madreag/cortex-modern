"""Require the original-activity codec checks from the runner's match selftest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from run_selftests import score_selftest
from run_sim_test import make_run


ROWS = ("rules_roundtrip", "rules_hash_sensitivity", "rules_legacy_defaults", "rules_invalid_refused")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=["codec", "launch"], required=True)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--port", type=int, default=48320)
    parser.add_argument("--variant", default="rules", choices=["rules", "default", "infinite", "site", "stock", "stock-scene", "brains", "brains-auto", "brains-shared", "hold-desync", "hold-resync", "resync-duel", "resync-skirmish", "brains-longname", "wire-refusal", "rendezvous-cap", "census", "missing-activity", "missing-scene", "missing-module", "missing-tech"])
    parser.add_argument("--dedicated", action="store_true")
    parser.add_argument("--captures", action="store_true", help="save the setup editor's UI captures for the visual review")
    parser.add_argument("--resolution", help="private window size for the captures, as WIDTHxHEIGHT")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--offline-repo", type=Path, help="repo whose exe runs the census case's offline -scenario arm")
    options = parser.parse_args()
    if options.resolution:
        options.resolution = tuple(int(part) for part in options.resolution.lower().split("x"))
    if options.case == "launch":
        from net_activity_launch import launch
        return launch(options)
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    run = make_run(options.repo, ["-net-match-selftest"], root / "net-match-selftest", options.timeout)
    try:
        record = run.start().finish()
    finally:
        run.close()
    stdout = (root / "net-match-selftest" / "stdout.log").read_text(errors="replace")
    result = score_selftest(stdout, record.get("exit_code"), record.get("timed_out"), "net-match-selftest")
    result["suite_pass"] = result["pass"]
    result["checks"] = {row: f"[net-match-selftest] PASS {row}" in stdout.splitlines() for row in ROWS}
    result["exe_sha256"] = record.get("exe_sha256")
    result["detector_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result["pass"] = result["pass"] and all(result["checks"].values())
    for row, ok in result["checks"].items():
        print(f"{'PASS' if ok else 'FAIL'} {row}: " + ("present" if ok else f"missing [net-match-selftest] PASS {row}"))
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
