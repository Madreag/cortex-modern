"""Discarded previews must leave the canonical world byte-identical.

Plays tools/fixtures/pickup_fire.ccreplay (a recorded two-peer duel: the dummy picks up the dropped
Battle Rifle at tick 144 and fires at 161) and at tick 153 runs and discards previews of depths 1, 4, 7 and 12, once
and three times each; every case must leave the canonical state as it found it, the Lua states included.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run  # noqa: E402

FIXTURES = ["pickup_fire.ccreplay", "pickup_fire.txt"]
CASES = "153:1,4,7,12:1,3"
EXPECTED_CASES = 8
ARGS = ["-net-replay", "tools/fixtures/pickup_fire.ccreplay", "-input-script", "tools/fixtures/pickup_fire.txt", "-tick-hashes",
        "-max-ticks", "221", "-num-lua-states", "4", "-local-prediction-depth", "7", "-local-prediction-invariance", CASES]
VERDICT = re.compile(r"^\[lpinv\] (PASS|FAIL) tick (\d+): (\d+)/(\d+) cases", re.M)
# The preview-hotplug row: the same replay with a gamepad plugged in at tick 60, while previews run every frame.
HOTPLUG_TICK = 60
HOTPLUG = re.compile(r"^\[hotplug\] tick=(\d+) pad=(\d+)$", re.M)


def sha256_of(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def score(stdout: str, exit_code, timed_out, hotplug: bool = False) -> dict:
    verdicts = VERDICT.findall(stdout or "")
    passed = [row for row in verdicts if row[0] == "PASS" and int(row[2]) == int(row[3]) == EXPECTED_CASES]
    fails = [line for line in (stdout or "").splitlines() if line.startswith("[lpinv] FAIL")]
    plugged = [row for row in HOTPLUG.findall(stdout or "") if int(row[0]) == HOTPLUG_TICK and int(row[1]) != 0]
    ok = exit_code == 0 and not timed_out and len(verdicts) == 1 and len(passed) == 1 and not fails and (not hotplug or len(plugged) == 1)
    reason = "" if ok else (f"exit_code={exit_code}" if exit_code != 0 else "timed_out" if timed_out else
                            (fails[0] if fails else f"verdicts={verdicts}" if len(passed) != 1 else "no pad arrived at the hot-plug tick"))
    return {"pass": ok, "exit_code": exit_code, "timed_out": timed_out, "verdicts": verdicts, "fail_lines": fails[:3], "reason": reason,
            **({"hotplug": plugged} if hotplug else {})}


def run_case(repo: Path, case: Path, timeout: float, hotplug: bool = False) -> dict:
    env = {"CCCP_TEST_HOTPLUG_PAD_TICK": str(HOTPLUG_TICK)} if hotplug else None
    run = make_run(repo, ARGS, case, timeout, env=env, fixtures=FIXTURES)
    try:
        record = run.start().finish()
    finally:
        run.close()
    stdout = (case / "stdout.log").read_text(encoding="utf-8", errors="replace") if (case / "stdout.log").exists() else ""
    scored = score(stdout, record.get("exit_code"), record.get("timed_out"), hotplug)
    scored["exe_sha256"] = record.get("exe_sha256")
    return scored


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=300)
    options = parser.parse_args()
    scored = run_case(options.repo, options.out, options.timeout)
    from verdict_artifact import write_verdict
    log = options.out / 'stdout.log'
    if not log.is_file(): log.write_text('runner produced no stdout log\n', encoding='utf-8')
    write_verdict(options.out / 'result.json', passed=scored['pass'],
                  counts=dict(expected_cases=EXPECTED_CASES, reported_summaries=len(scored['verdicts']),
                              passed_runs=int(scored['pass']), failed_runs=int(not scored['pass'])),
                  inputs=[Path(__file__), *(options.repo / 'tools/fixtures' / leaf for leaf in FIXTURES)],
                  log=log, score=scored)
    print(json.dumps(scored, indent=2))
    return 0 if scored["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
