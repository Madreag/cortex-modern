from pathlib import Path
import hashlib
import subprocess
import sys

out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w3-comparator")
src = Path(r"D:\Projects\reviews\takeover-20260909\source41-matrix-review\snapshot-inventory-roles-20260910")
pair = [
    Path(r"D:\mx\s41r2\mp_snapshot_p5\fresh\e2e\snapshot_p5\p5snap_p1.ccsave"),
    Path(r"D:\mx\s41r2\mp_snapshot_p5\fresh\e2e\snapshot_p5\p5snap_p2.ccsave"),
]
python = sys.executable
commands = []


def run(label, argv, cwd, log_path):
    recorded = {
        "label": label,
        "argv": argv,
        "cwd": str(cwd) if cwd else None,
        "log": str(log_path),
    }
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log_path.write_text(result.stdout, encoding="utf-8")
    recorded["returncode"] = result.returncode
    commands.append(recorded)
    print(f"{label} returncode={result.returncode} log={log_path}")
    return result


pair_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in pair}
(out / "retained-pair-hashes.txt").write_text(
    "\n".join(f"{digest} {path}" for path, digest in pair_hashes.items()) + "\n",
    encoding="utf-8",
)

for version, log_name in (("repaired", "tests-repaired.txt"), ("control", "tests-control.txt")):
    run(
        f"{version}-tests",
        [python, "-B", "-m", "unittest", "test_snapshot_inventory_roles", "test_compare_snapshots", "-v"],
        src / version,
        out / log_name,
    )

for version in ("control", "repaired"):
    report = out / f"retained-{version}.json"
    log = out / f"retained-{version}.log"
    run(
        f"{version}-pair",
        [python, "-B", str(src / version / "compare_snapshots.py"), str(pair[0]), str(pair[1]), "--report", str(report)],
        src / version,
        log,
    )

after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in pair}
(out / "commands.json").write_text(
    __import__("json").dumps(
        {
            "python": python,
            "commands": commands,
            "pair_hashes_before": pair_hashes,
            "pair_hashes_after": after,
            "pair_unchanged": pair_hashes == after,
        },
        indent=2,
    )
    + "\n",
    encoding="utf-8",
)
print("pair_unchanged", pair_hashes == after)
