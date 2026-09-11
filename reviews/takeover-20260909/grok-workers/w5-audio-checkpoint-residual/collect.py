"""Read-only evidence collector for W5. Writes only under this directory."""
from __future__ import annotations

import json
import os
import re
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w5-audio-checkpoint-residual")
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
SKIP_DIR_NAMES = {
    "Data",
    "external",
    "modules",
    "$Recycle.Bin",
    "System Volume Information",
}
NEEDLE_A = "has no registered owner"
NEEDLE_B = "past the sound container cursor"
TEXT_EXTS = {".log", ".txt", ".json"}
SNAP_HINTS = (
    "snap",
    "checkpoint",
    "journal",
    "resync",
    "savedgame",
    "savegame",
    ".ccn",
    ".ccs",
)
JOB = Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527")


def is_reparse(entry: os.DirEntry) -> bool:
    if entry.is_symlink():
        return True
    try:
        st = entry.stat(follow_symlinks=False)
    except OSError:
        return True
    attrs = getattr(st, "st_file_attributes", 0)
    return bool(attrs & FILE_ATTRIBUTE_REPARSE_POINT)


def safe_stat(path: Path) -> os.stat_result | None:
    try:
        return os.stat(path, follow_symlinks=False)
    except OSError:
        return None


def walk_skip_reparse(root: Path, file_filter=None):
    if not root.exists():
        return
    stack = [root]
    while stack:
        current = stack.pop()
        try:
            with os.scandir(current) as it:
                entries = list(it)
        except OSError as exc:
            yield ("error", current, str(exc))
            continue
        for entry in entries:
            if is_reparse(entry):
                yield ("reparse", Path(entry.path), None)
                continue
            name = entry.name
            if entry.is_dir(follow_symlinks=False):
                if name in SKIP_DIR_NAMES:
                    yield ("skipdir", Path(entry.path), None)
                    continue
                stack.append(Path(entry.path))
                continue
            if file_filter and not file_filter(entry):
                continue
            yield ("file", Path(entry.path), entry)


def inventory_job():
    rows = []
    skipped = []
    errors = []
    all_files = []
    for kind, path, entry in walk_skip_reparse(JOB):
        if kind == "reparse":
            skipped.append({"path": str(path), "reason": "reparse"})
            continue
        if kind == "skipdir":
            skipped.append({"path": str(path), "reason": "named-skip"})
            continue
        if kind == "error":
            errors.append({"path": str(path), "error": entry})
            continue
        st = safe_stat(path)
        if st is None:
            errors.append({"path": str(path), "error": "stat-failed"})
            continue
        rec = {
            "path": str(path),
            "rel": str(path.relative_to(JOB)),
            "name": path.name,
            "size": st.st_size,
            "suffix": path.suffix.lower(),
        }
        all_files.append(rec)
        low = rec["rel"].lower()
        name_low = rec["name"].lower()
        interesting = False
        if any(h in low for h in SNAP_HINTS):
            interesting = True
        if "p5resync" in name_low or "saved" in name_low:
            interesting = True
        if rec["suffix"] in {".ini"} and "usersavedgames" in low:
            interesting = True
        if rec["name"] in {
            "verdict.json",
            "drop3_returner_report.json",
            "drop3_host_report.json",
            "drop3_client2_report.json",
            "drop3_client1_report.json",
        }:
            interesting = True
        if interesting:
            rows.append(rec)
    rows.sort(key=lambda r: r["rel"])
    all_files.sort(key=lambda r: r["rel"])
    (OUT / "job_inventory.json").write_text(
        json.dumps(
            {
                "job": str(JOB),
                "file_count_no_reparse": len(all_files),
                "interesting": rows,
                "skipped": skipped,
                "errors": errors,
                "top_level": [
                    {"name": p.name, "is_dir": p.is_dir(), "size": (safe_stat(p).st_size if safe_stat(p) else None)}
                    for p in sorted(JOB.iterdir(), key=lambda p: p.name)
                    if not (p.is_symlink() or (getattr(safe_stat(p), "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT))
                ],
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    return rows, all_files, skipped


def grep_file(path: Path, max_bytes: int, needles: list[str]):
    st = safe_stat(path)
    if st is None or st.st_size > max_bytes:
        return []
    hits = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    for i, line in enumerate(text.splitlines(), 1):
        for needle in needles:
            if needle in line:
                hits.append(
                    {
                        "file": str(path),
                        "line": i,
                        "needle": needle,
                        "text": line[:400],
                    }
                )
    return hits


def classify_hit(path: Path) -> dict:
    parts = path.parts
    job = ""
    gate = ""
    arm = ""
    peer = ""
    for i, part in enumerate(parts):
        if part.startswith("peers_3_4_regression") or part.startswith("j") and i + 1 < len(parts):
            pass
        if "peers_3_4_regression" in part:
            gate = "peers_3_4_regression"
            job = part
        if re.fullmatch(r"j\d+", part):
            arm = part
        if part.startswith("drop3_") or part.startswith("peers3_") or part.startswith("peers4_"):
            peer = part
            if peer.endswith("_report.json") or peer.endswith("_trace.json"):
                peer = peer.split("_report")[0].split("_trace")[0]
        if part in {"stdout.log", "LogConsole.txt"} and i >= 1:
            if not peer:
                peer = parts[i - 1] if parts[i - 1] != "runtime" else (parts[i - 2] if i >= 2 else "")
    # reviews / resume
    if "reviews" in parts or path.name == "RESUME.md":
        gate = gate or "review-doc"
    return {"job": job, "gate": gate, "arm": arm, "peer": peer}


def search_root(root: Path, max_bytes: int, restrict_ext: bool, label: str):
    hits = []
    scanned = 0
    skipped_large = 0
    errors = 0
    if not root.exists():
        return {
            "root": str(root),
            "present": False,
            "hits": [],
            "scanned": 0,
            "skipped_large": 0,
            "errors": 0,
        }

    def file_filter(entry: os.DirEntry) -> bool:
        if not restrict_ext:
            return True
        return Path(entry.name).suffix.lower() in TEXT_EXTS

    for kind, path, entry in walk_skip_reparse(root, file_filter=file_filter if restrict_ext else None):
        if kind != "file":
            continue
        if restrict_ext and path.suffix.lower() not in TEXT_EXTS:
            continue
        st = safe_stat(path)
        if st is None:
            errors += 1
            continue
        if st.st_size > max_bytes:
            skipped_large += 1
            continue
        scanned += 1
        for hit in grep_file(path, max_bytes, [NEEDLE_A, NEEDLE_B]):
            hit.update(classify_hit(path))
            hit["root"] = label
            hits.append(hit)
    return {
        "root": str(root),
        "present": True,
        "hits": hits,
        "scanned": scanned,
        "skipped_large": skipped_large,
        "errors": errors,
    }


def find_peers_jobs(roots: list[Path]):
    found = []
    for root in roots:
        if not root.exists():
            found.append({"root": str(root), "present": False, "jobs": []})
            continue
        jobs = []
        # shallow: jXX/peers_3_4_regression_* and any direct peers_3_4_*
        try:
            with os.scandir(root) as it:
                level1 = list(it)
        except OSError as exc:
            found.append({"root": str(root), "present": True, "error": str(exc), "jobs": []})
            continue
        for entry in level1:
            if is_reparse(entry):
                continue
            p = Path(entry.path)
            if entry.is_dir(follow_symlinks=False):
                if "peers_3_4_regression" in entry.name:
                    jobs.append(str(p))
                    continue
                try:
                    with os.scandir(p) as it2:
                        for e2 in it2:
                            if is_reparse(e2):
                                continue
                            if e2.is_dir(follow_symlinks=False) and "peers_3_4_regression" in e2.name:
                                jobs.append(e2.path)
                except OSError:
                    pass
        found.append({"root": str(root), "present": True, "jobs": sorted(jobs)})
    return found


def extract_log_events(path: Path):
    if not path.exists():
        return {"path": str(path), "missing": True, "lines": []}
    text = path.read_text(encoding="utf-8", errors="replace")
    keys = (
        "snapbench",
        "net-match",
        "audio-checkpoint",
        "[audio]",
        "music-checkpoint",
        "gui-sound-checkpoint",
        "runtime-globals",
        "scriptgraph",
        "Game saved",
        "resync",
        "reseat",
        "rejoin",
        "state transfer",
        "launching from",
        "could not launch",
    )
    lines = []
    for i, line in enumerate(text.splitlines(), 1):
        if any(k.lower() in line.lower() for k in keys):
            lines.append({"n": i, "text": line})
    return {"path": str(path), "missing": False, "size": path.stat().st_size, "lines": lines}


def compare_repeats():
    repeats = {
        1: Path(r"D:\mx\s41b3\j38\peers_3_4_regression_20260910_022228"),
        2: Path(r"D:\mx\s41b3\j39\peers_3_4_regression_20260910_022357"),
        3: Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527"),
        4: Path(r"D:\mx\s41b3\j41\peers_3_4_regression_20260910_022655"),
        5: Path(r"D:\mx\s41b3\j42\peers_3_4_regression_20260910_022824"),
    }
    out = {}
    for n, root in repeats.items():
        rec = {"root": str(root), "exists": root.exists()}
        if root.exists():
            verdict = root / "verdict.json"
            if verdict.exists() and verdict.stat().st_size < 20_000_000:
                v = json.loads(verdict.read_text(encoding="utf-8"))
                rec["verdict_passed"] = v.get("passed")
                rec["exe"] = v.get("executable_sha256")
                rec["built_from"] = v.get("built_from")
            rec["returner_stdout"] = extract_log_events(root / "drop3_returner" / "stdout.log")
            rec["returner_console"] = extract_log_events(root / "drop3_returner" / "runtime" / "LogConsole.txt")
            rec["host_stdout"] = extract_log_events(root / "drop3_host" / "stdout.log")
            rec["host_console"] = extract_log_events(root / "drop3_host" / "runtime" / "LogConsole.txt")
            rec["client2_stdout"] = extract_log_events(root / "drop3_client2" / "stdout.log")
            rec["client1_stdout"] = extract_log_events(root / "drop3_client1" / "stdout.log")
            rec["returner_report"] = None
            rp = root / "drop3_returner_report.json"
            if rp.exists() and rp.stat().st_size < 2_000_000:
                rec["returner_report"] = json.loads(rp.read_text(encoding="utf-8"))
        out[str(n)] = rec
    (OUT / "repeat_compare.json").write_text(json.dumps(out, indent=2), encoding="utf-8")
    return out


def package_sizes():
    paths = [
        Path(r"D:\Projects\p4b-interp-validation\Cortex Command.exe"),
        Path(r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\Cortex Command.exe"),
        Path(r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\Cortex Command.pdb"),
        Path(r"D:\Projects\reviews\recovery-2026-09-07\contract-audit\grouped-build-bb3cf264\build.json"),
        Path(r"D:\mx\s41b3\pin.json"),
        Path(r"D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\h4gates\peers_3_4_regression.py"),
        Path(r"D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\driver\h4gates\common.py"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\verdict.json"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner\stdout.log"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner\runtime\LogConsole.txt"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_host\stdout.log"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_host\runtime\LogConsole.txt"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_client2\stdout.log"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_client1\stdout.log"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner_report.json"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_host_report.json"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_client2_report.json"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_host_trace.json"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_client2_trace.json"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner\launch.json"),
        Path(r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_host\launch.json"),
        Path(r"D:\mx\s41b3\plan.json"),
    ]
    rows = []
    for p in paths:
        st = safe_stat(p)
        rows.append({"path": str(p), "exists": p.exists(), "size": None if st is None else st.st_size})
    return rows


def search_reviews():
    hits = []
    roots = [
        Path(r"D:\Projects\reviews"),
        Path(r"D:\Projects\RESUME.md"),
    ]
    for root in roots:
        if root.is_file():
            for hit in grep_file(root, 80_000_000, [NEEDLE_A, NEEDLE_B, "audio-checkpoint", "voice 36 has no registered owner"]):
                if NEEDLE_A in hit["text"] or NEEDLE_B in hit["text"]:
                    hit.update(classify_hit(root))
                    hit["root"] = "docs"
                    hits.append(hit)
            continue
        for kind, path, entry in walk_skip_reparse(root):
            if kind != "file":
                continue
            if path.suffix.lower() != ".md":
                continue
            st = safe_stat(path)
            if st is None or st.st_size > 80_000_000:
                continue
            for hit in grep_file(path, 80_000_000, [NEEDLE_A, NEEDLE_B]):
                hit.update(classify_hit(path))
                hit["root"] = "reviews"
                hits.append(hit)
    return hits


def main():
    interesting, all_files, skipped = inventory_job()
    peers_jobs = find_peers_jobs(
        [
            Path(r"D:\mx\s41b3"),
            Path(r"D:\mx\s41"),
            Path(r"D:\mx\s40lanes"),
        ]
    )
    (OUT / "peers_jobs.json").write_text(json.dumps(peers_jobs, indent=2), encoding="utf-8")

    history = {
        "s41b3": search_root(Path(r"D:\mx\s41b3"), 20_000_000, True, "s41b3"),
        "s41": search_root(Path(r"D:\mx\s41"), 20_000_000, True, "s41"),
        "s40lanes": search_root(Path(r"D:\mx\s40lanes"), 20_000_000, True, "s40lanes"),
    }
    # strip huge nested by writing hits only plus counts
    slim = {}
    for key, rec in history.items():
        slim[key] = {
            "root": rec["root"],
            "present": rec["present"],
            "scanned": rec.get("scanned"),
            "skipped_large": rec.get("skipped_large"),
            "errors": rec.get("errors"),
            "hit_count": len(rec.get("hits", [])),
            "hits": rec.get("hits", []),
        }
    review_hits = search_reviews()
    slim["reviews_md"] = {"hit_count": len(review_hits), "hits": review_hits}
    (OUT / "history_hits.json").write_text(json.dumps(slim, indent=2), encoding="utf-8")

    compare_repeats()
    pkg = package_sizes()
    (OUT / "package_sizes.json").write_text(json.dumps(pkg, indent=2), encoding="utf-8")

    # classify how many peers_3_4 jobs show the class
    class_jobs = set()
    for rec in slim.values():
        for hit in rec.get("hits", []):
            if hit.get("gate") == "peers_3_4_regression":
                class_jobs.add(hit.get("job"))
    (OUT / "history_summary.json").write_text(
        json.dumps(
            {
                "peers_jobs": peers_jobs,
                "jobs_with_class_needles": sorted(class_jobs),
                "interesting_count": len(interesting),
                "job_file_count_no_reparse": len(all_files),
                "skipped": skipped[:200],
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print("done")
    print("interesting", len(interesting))
    print("job_files", len(all_files))
    print("hits s41b3", slim["s41b3"]["hit_count"])
    print("hits s41", slim["s41"]["hit_count"])
    print("hits s40lanes", slim["s40lanes"]["hit_count"])
    print("hits reviews", slim["reviews_md"]["hit_count"])
    print("peers_jobs", json.dumps(peers_jobs))


if __name__ == "__main__":
    main()
