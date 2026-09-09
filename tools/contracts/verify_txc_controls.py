"""Check a transaction control archive the way prepare_transaction_control_commands.py does, and optionally prove the
generator is content-deterministic by comparing it with a second graph_controls.py run into another directory.

Two runs of the generator never hash the same: zipfile.writestr stamps each entry with the wall clock. Content is what
has to match, so the entries are compared, not the archives.
"""

from pathlib import Path
import argparse
import hashlib
import json
import zipfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controls", type=Path, required=True)
    parser.add_argument(
        "--compare",
        type=Path,
        help="an independent graph_controls.py run over the same seed",
    )
    options = parser.parse_args()
    manifest = json.loads((options.controls / "manifest.json").read_text())

    report = {
        "controls": str(options.controls),
        "compare": str(options.compare) if options.compare else None,
        "hashes": [],
        "content": [],
    }
    for case in manifest["cases"]:
        if "unavailable" in case:
            report["hashes"].append(
                {"name": case["name"], "unavailable": case["unavailable"]}
            )
            continue
        path = options.controls / (case["name"] + ".ccsave")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        report["hashes"].append(
            {
                "name": case["name"],
                "manifest": case["sha256"],
                "actual": actual,
                "match": actual == case["sha256"],
                "bytes": path.stat().st_size,
            }
        )
        if not options.compare:
            continue
        other = options.compare / (case["name"] + ".ccsave")
        if not other.exists():
            report["content"].append({"name": case["name"], "missing": str(other)})
            continue
        with zipfile.ZipFile(path) as a, zipfile.ZipFile(other) as b:
            names = (
                [item.filename for item in a.infolist()],
                [item.filename for item in b.infolist()],
            )
            report["content"].append(
                {
                    "name": case["name"],
                    "entries": len(names[0]),
                    "same_names": names[0] == names[1],
                    "differing_entries": [
                        n for n in names[0] if a.read(n) != b.read(n)
                    ],
                }
            )

    report["all_hashes_match"] = all(
        item.get("match", True) for item in report["hashes"]
    )
    report["all_content_identical"] = (not options.compare) or all(
        item.get("same_names") and not item["differing_entries"]
        for item in report["content"]
    )
    report["cases"] = len(
        [case for case in manifest["cases"] if "unavailable" not in case]
    )
    report["unavailable"] = [
        case["name"] for case in manifest["cases"] if "unavailable" in case
    ]
    print(json.dumps(report, indent=2))
    return 0 if report["all_hashes_match"] and report["all_content_identical"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
