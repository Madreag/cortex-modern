"""Require the host Kick/Ban codec and removal arms from the engine selftests."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from run_selftests import score_selftest
from run_sim_test import make_run


CODEC_ROWS = (
    "removal-codec: host removal roundtrips; forged old-wire stamps refused",
    "removal-codec: forged/remapped/wrong-epoch/duplicate refused; accept is terminal",
)
KICK_ROWS = (
    "kick: socket-only drop still opens a reclaim hold",
    "kick: targeted peer removed with no reclaim hold",
    "kick: service RemoveParticipant broadcasts, evicts and disconnects",
    "kick: a removed link loses its seat, its transactions and its binding",
    "kick: Starting removals and unbans marshal onto the setup worker in order",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=["removal-codec", "kick", "menus"], required=True)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--port", type=int, default=48340)
    parser.add_argument("--size", default="640x360")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    if options.case == "menus":
        result = {"pass": False, "reason": "the multiplayer lobby owns the Kick/Ban menus"}
        (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps(result, indent=2))
        return 1
    flags = ["-net-protocol-selftest"] if options.case == "removal-codec" else ["-net-reconnect-session-selftest", "-net-match-selftest"]
    extra = ["-net-reconnect-session-selftest"] if options.case == "removal-codec" else []
    rows = CODEC_ROWS if options.case == "removal-codec" else KICK_ROWS
    stdout = ""
    last_record = {}
    suite_ok = True
    for flag in flags + extra:
        run_name = flag.lstrip("-")
        run = make_run(options.repo, [flag], root / run_name, options.timeout)
        try:
            last_record = run.start().finish()
        finally:
            run.close()
        piece_out = (root / run_name / "stdout.log").read_text(errors="replace")
        stdout += piece_out
        piece = score_selftest(piece_out, last_record.get("exit_code"), last_record.get("timed_out"), run_name)
        if not piece["pass"]:
            suite_ok = False
    result = {"pass": suite_ok, "reason": "" if suite_ok else "a tagged make_run failed"}
    result["suite_pass"] = suite_ok
    result["checks"] = {row: f"PASS {row}" in stdout for row in rows}
    result["exe_sha256"] = last_record.get("exe_sha256")
    result["detector_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result["pass"] = suite_ok and all(result["checks"].values())
    for row, ok in result["checks"].items():
        print(f"{'PASS' if ok else 'FAIL'} {row}: " + ("present" if ok else "missing"))
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
