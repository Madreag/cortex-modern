"""Reparse-safe search for prediction_executed and heal result check names. Read-only."""
from __future__ import annotations

import json
import os
import stat
from pathlib import Path

LIMIT = 20 * 1024 * 1024
EXTS = {".json", ".log", ".txt"}
SKIP_DIR_NAMES = {"Data", "external", "modules"}


def is_reparse(path: Path) -> bool:
    try:
        return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except OSError:
        return False


def walk_files(root: Path):
    if not root.exists():
        return
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        current = Path(dirpath)
        if is_reparse(current) and current != root:
            dirnames[:] = []
            continue
        dirnames[:] = [
            name for name in dirnames
            if name not in SKIP_DIR_NAMES and not is_reparse(current / name)
        ]
        for name in filenames:
            path = current / name
            if path.suffix.lower() not in EXTS or is_reparse(path):
                continue
            try:
                if path.stat().st_size > LIMIT:
                    continue
            except OSError:
                continue
            yield path


def hits_in(root: Path, needle: str, cap=80):
    found = []
    for path in walk_files(root):
        try:
            text = path.read_text(encoding="utf-8-sig", errors="replace")
        except OSError:
            continue
        if needle not in text:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            if needle in line:
                found.append(f"{path}:{i}: {line.strip()[:240]}")
                if len(found) >= cap:
                    return found
    return found


def check_names(path: Path):
    if not path.is_file():
        return None
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    checks = data.get("checks")
    if isinstance(checks, list):
        return [(c.get("name"), c.get("status"), str(c.get("detail", ""))[:160]) for c in checks]
    if isinstance(checks, dict):
        return list(checks.items())
    return None


def local_prediction(path: Path):
    if not path.is_file():
        return None
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    return data.get("local_prediction")


OUT = Path(__file__).with_name("heal-prediction-search.json")
roots = [
    Path(r"D:\mx\s40lanes"),
    Path(r"D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes"),
    Path(r"D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates"),
]
# Only search named heal result files under expanded-mod-gates to avoid Data.
heal_results = []
for root in (
    Path(r"D:\Projects\reviews\recovery-2026-09-06\expanded-mod-gates"),
    Path(r"D:\Projects\reviews\claude-review-2026-09-08\lanes"),
    Path(r"D:\mx\s40lanes"),
):
    if not root.exists():
        continue
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        current = Path(dirpath)
        if is_reparse(current) and current != root:
            dirnames[:] = []
            continue
        dirnames[:] = [
            name for name in dirnames
            if name not in SKIP_DIR_NAMES and not is_reparse(current / name)
            and "runtime" not in name.lower()
        ]
        if "heal" in current.name.lower() and "result.json" in filenames:
            heal_results.append(current / "result.json")

s40_hits = hits_in(Path(r"D:\mx\s40lanes"), "prediction_executed")
report_hits = []
report = Path(r"D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\report.md")
if report.is_file():
    for i, line in enumerate(report.read_text(encoding="utf-8-sig", errors="replace").splitlines(), 1):
        if "prediction_executed" in line or ("heal" in line.lower() and "PASS" in line):
            report_hits.append(f"{report}:{i}: {line.strip()[:240]}")

named = {
    "s41b4": Path(r"D:\mx\s41b4\j73\result.json"),
    "s41b4_host": Path(r"D:\mx\s41b4\j73\fresh\e2e\resync_heal\host_report.json"),
    "s41b4_client": Path(r"D:\mx\s41b4\j73\fresh\e2e\resync_heal\client_report.json"),
}

heal_summaries = []
for path in heal_results[:40]:
    names = check_names(path)
    if names is None:
        continue
    has_pred = any((row[0] if isinstance(row, tuple) else row) == "prediction_executed" for row in names)
    heal_summaries.append({
        "path": str(path),
        "has_prediction_executed": has_pred,
        "names": [row[0] if isinstance(row, tuple) else row for row in names],
    })

payload = {
    "s40lanes_prediction_executed_hits": s40_hits,
    "heal_result_files_found": [str(p) for p in heal_results],
    "heal_result_check_summaries": heal_summaries,
    "s41b4_check_names": check_names(named["s41b4"]),
    "s41b4_host_local_prediction": local_prediction(named["s41b4_host"]),
    "s41b4_client_local_prediction": local_prediction(named["s41b4_client"]),
}
OUT.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print("heal_results", len(heal_results))
print("s40_hits", len(s40_hits))
print("heal_with_prediction_executed", sum(1 for row in heal_summaries if row["has_prediction_executed"]))
print("wrote", OUT)
