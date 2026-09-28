"""Classify a passing intentional-assert row's structured summary as an observation."""

import argparse
import json
from pathlib import Path


def expected_assert(row: dict) -> bool:
    return (
        row.get("selftest") == "headless-assert-continues"
        and row.get("pass") is True
        and row.get("exit_code") == 1
        and row.get("timed_out") is False
        and row.get("fault_line", "").startswith("RTE Assert (headless, continued like Ignore): Assertion in file '")
        and "Source/Menus/MenuAutomation.cpp'" in row.get("fault_line", "")
        and row.get("verdicts_after_assert") == ["[menu-script] assert_screen expected=MainScreen actual=MainScreen PASS"]
        and row.get("shutdown_line") == "[assert] the run continued past an assert; see the RTE Assert line above"
        and bool(row.get("exe_sha256"))
    )


def normalize(document: dict, root: Path) -> dict:
    accepted = set()
    for path in root.glob("suite*/result.json"):
        row = json.loads(path.read_text()).get("results", {}).get("headless-assert-continues", {})
        if expected_assert(row):
            accepted.add(row["exe_sha256"])
    defects = []
    observations = list(document.get("observations", []))
    for defect in document["defects"]:
        expected = False
        if defect.get("kind") == "assert-dialog" and accepted:
            try:
                filename, line = defect["path"].rsplit(":", 1)
                source = Path(filename).resolve()
                source.relative_to(root.resolve())
                record = json.loads(source.read_text().splitlines()[int(line) - 1])
                expected = expected_assert(record) and record["exe_sha256"] in accepted
            except (OSError, ValueError, KeyError, IndexError, TypeError):
                pass
        if expected:
            observations.append({**defect, "kind": "expected-assert", "original_kind": defect["kind"],
                                 "reason": "The intentional assert row passed its continuation and nonzero-exit checks."})
        else:
            defects.append(defect)
    return {**document, "defects": defects, "observations": observations, "defect_count": len(defects),
            "hard_count": sum(item["kind"] != "soft" for item in defects)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    before = json.loads(args.input.read_text())
    after = normalize(before, args.root)
    args.out.write_text(json.dumps(after, indent=2) + "\n")
    print(f"defects={after['defect_count']} hard={after['hard_count']} expected_asserts={before['defect_count'] - after['defect_count']}")
