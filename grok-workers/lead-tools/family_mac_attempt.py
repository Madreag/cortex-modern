"""Drive one extra Mac attempt of a family through the family's own launch/finish steps, into a SEPARATE record.

  python family_mac_attempt.py --source 44 --mac-attempt 2 --launch    # stage, upload, start (record: family-source44-mac2.json)
  python family_mac_attempt.py --source 44 --mac-attempt 2 --finish    # wait for the remote report, verify, record

The family's own record (family-source44.json) is never touched here; the lead appends the two steps to it by hand
once run_family.py has finished, quoting this record. Preconditions: the remote attempt dir must not exist for --launch.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

CONTRACT = Path("D:/Projects/reviews/recovery-2026-09-07/contract-audit")
sys.path.insert(0, str(CONTRACT))
import run_family  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=int, required=True)
    parser.add_argument("--mac-attempt", type=int, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--launch", action="store_true")
    mode.add_argument("--finish", action="store_true")
    options = parser.parse_args()
    family = run_family.Family(options.source, None, "lead-mac-attempt")
    family.mac_attempt = options.mac_attempt
    family.summary_path = CONTRACT / f"family-source{options.source}-mac{options.mac_attempt}.json"
    family.log = CONTRACT / f"family-source{options.source}-mac{options.mac_attempt}.log"
    if family.summary_path.exists():
        family.summary = json.loads(family.summary_path.read_text(encoding="utf-8"))
        family.summary.pop("finished", None)
    elif options.finish:
        print("no launch record for this attempt; run --launch first", file=sys.stderr)
        return 2
    passed = False
    try:
        if options.launch:
            family.note(f"lead: Mac attempt {options.mac_attempt} launched by family_mac_attempt.py")
            family.mac()
            launches = [s for s in family.summary["steps"] if s["step"] == "mac_launch"]
            passed = bool(launches) and launches[-1]["exit"] == 0
        else:
            family.note(f"lead: Mac attempt {options.mac_attempt} completion read by family_mac_attempt.py")
            passed = family.mac_finish()
    except Exception as error:  # the record keeps the failure
        family.record("mac_launch" if options.launch else "mac_complete", 1, error=f"{type(error).__name__}: {error}")
    family.summary["finished"] = family.now()
    family.summary_path.write_text(json.dumps(family.summary, indent=2), encoding="utf-8")
    print(json.dumps({"mac_attempt": options.mac_attempt, "mode": "launch" if options.launch else "finish", "passed": passed,
                      "record": str(family.summary_path)}))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
