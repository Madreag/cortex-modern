"""Run the committed leave handoff and exact reseat arm through the isolated runner."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re

from run_sim_test import make_run


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--expect", choices=("red", "green"), required=True)
    parser.add_argument("--timeout", type=int, default=300)
    options = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("the verification family owns the machine")
    repo, out = options.repo.resolve(), options.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    executable = repo / "Cortex Command.exe"
    with executable.open("rb") as handle:
        executable_sha256 = hashlib.file_digest(handle, "sha256").hexdigest()
    os.environ["CCCP_HEADLESS"] = "1"
    run = make_run(repo, ["-net-lockstep-selftest"], out / "run", options.timeout,
                   env={"CCCP_HEADLESS": "1"})
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = out / "run/stdout.log"
    text = log.read_text(encoding="utf-8-sig", errors="replace")
    checks = []
    evidence = []
    for policy, departure, teammate in itertools.product(("team", "host_cpu"), ("leave", "drop"), (0, 1)):
        name = f"leave_ai_takeover policy={policy} departure={departure} teammate={teammate}"
        if options.expect == "green":
            checks.append(f"[net-lockstep-selftest] PASS {name}\n" in text)
        for peer in (1, 3):
            rows = []
            for number, line in enumerate(text.splitlines(), 1):
                if f"{name} peer={peer} " not in line:
                    continue
                match = re.search(r"frame=(\d+) leave_frame=(\d+) actor=(\d+) uid=(\d+) \{input_mode=(\d+)", line)
                if match and int(match[1]) == int(match[2]) + 1:
                    rows.append((number, line, int(match[3]), int(match[5])))
            checks.append(len(rows) == 4 and {row[2] for row in rows} == {0, 1, 2, 3})
            if options.expect == "red":
                checks.append(any(" FAIL " in line and actor == 0 and mode == 1 for _, line, actor, mode in rows))
            else:
                checks.append(all(" PASS " in line for _, line, _, _ in rows))
            evidence.extend({"path": str(log), "line": number, "text": line} for number, line, _, _ in rows)
    checks.append(not record.get("timed_out") and record.get("evidence_complete", False))
    if options.expect == "red":
        checks.append(record.get("exit_code") == 1)
        checks.append("RTE Abort" not in text and "RTE Assert" not in text)
    else:
        checks.append(record.get("exit_code") == 0 and "[net-lockstep-selftest] PASS\n" in text)
        checks.append(not re.search(r"\bFAIL\b|RTE Abort|RTE Assert|stack traceback", text))
    result = {"expect": options.expect, "matched": all(checks), "checks": checks,
              "exe_sha256": executable_sha256, "exit_code": record.get("exit_code"),
              "evidence": evidence, "runner": str(out / "run")}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key not in ("checks", "evidence")}))
    return 0 if result["matched"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
