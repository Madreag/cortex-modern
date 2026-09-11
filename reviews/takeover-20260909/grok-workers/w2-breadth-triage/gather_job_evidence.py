"""Read-only: quote oracles, list Rejected-a hits, dump key job files."""
import json
import os
import re
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")
ROOT = Path(r"D:\mx\s41b3")

# --- fl200 ---
fl_files = [
    ROOT / "j28" / "fl" / "fl200" / "result.json",
    ROOT / "j28" / "fl" / "fl200" / "host_report.json",
    ROOT / "j28" / "fl" / "fl200" / "client_report.json",
    ROOT / "logs" / "fl200.log",
]
for p in fl_files:
    dest = OUT / ("quote_fl200_" + "_".join(p.parts[-3:]))
    if p.exists():
        dest.write_text(p.read_text(encoding="utf-8", errors="replace"), encoding="utf-8")
    else:
        dest.write_text(f"MISSING {p}\n", encoding="utf-8")

# --- Rejected a across all j* ---
pat = re.compile(r"Rejected a", re.I)
hits = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    # do not follow? os.walk follows by default; junctions could be huge.
    # skip runtime Data junctions if possible by not descending into Data
    dirnames[:] = [d for d in dirnames if d.lower() not in ("data", "external", "modules", "node_modules")]
    for fn in filenames:
        if not fn.lower().endswith((".txt", ".log", ".json", ".out")):
            continue
        fp = Path(dirpath) / fn
        try:
            text = fp.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if "Rejected a" in line:
                lo = max(0, i - 5)
                hi = min(len(lines), i + 6)
                ctx = []
                for j in range(lo, hi):
                    ctx.append(f"{j+1}|{lines[j]}")
                rel = str(fp)
                peer = None
                parts = fp.parts
                for idx, part in enumerate(parts):
                    if part.startswith("j") and idx + 1 < len(parts):
                        # next dirs after job often include run name then peer
                        pass
                hits.append(
                    {
                        "path": rel,
                        "line": i + 1,
                        "text": line,
                        "context": ctx,
                    }
                )

(OUT / "rejected_a_hits.json").write_text(json.dumps(hits, indent=2), encoding="utf-8")
print("Rejected a hits", len(hits))
for h in hits:
    print(h["path"], h["line"], h["text"][:120])

# --- copy key verdict/result/summary/logs for nonpass ---
wanted = {
    "fl200": [
        r"D:\mx\s41b3\logs\fl200.log",
        r"D:\mx\s41b3\j28\fl\fl200\result.json",
    ],
    "h4_clean_leave_1": [
        r"D:\mx\s41b3\logs\h4_clean_leave_1.log",
        r"D:\mx\s41b3\j33\clean_leave_20260910_021812\verdict.json",
        r"D:\mx\s41b3\j33\clean_leave_20260910_021812\ambiguous_host\runtime\LogConsole.txt",
    ],
    "h4_reclaim_socket_1": [
        r"D:\mx\s41b3\logs\h4_reclaim_socket_1.log",
        r"D:\mx\s41b3\j34\reclaim_socket_20260910_021916\verdict.json",
        r"D:\mx\s41b3\j34\reclaim_socket_20260910_021916\host\runtime\LogConsole.txt",
    ],
    "h4_fencing_two_transports_1": [
        r"D:\mx\s41b3\logs\h4_fencing_two_transports_1.log",
        r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\verdict.json",
        r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\host\runtime\LogConsole.txt",
        r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\client2\stdout.log",
        r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\host\stdout.log",
    ],
    "h4_rejoin_after_resync_1": [
        r"D:\mx\s41b3\logs\h4_rejoin_after_resync_1.log",
        r"D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132\verdict.json",
        r"D:\mx\s41b3\j37\rejoin_after_resync_20260910_022132\resync_host\runtime\LogConsole.txt",
    ],
    "h4_peers_3_4_regression_3": [
        r"D:\mx\s41b3\logs\h4_peers_3_4_regression_3.log",
        r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\verdict.json",
        r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner\runtime\LogConsole.txt",
        r"D:\mx\s41b3\j40\peers_3_4_regression_20260910_022527\drop3_returner\stdout.log",
    ],
    "b1_substitute_commit": [
        r"D:\mx\s41b3\logs\b1_substitute_commit.log",
        r"D:\mx\s41b3\j43\substitute_commit_20260910_022955\verdict.json",
    ],
    "b1_substitute_returner_wins": [
        r"D:\mx\s41b3\logs\b1_substitute_returner_wins.log",
        r"D:\mx\s41b3\j44\substitute_returner_wins_20260910_023039\verdict.json",
    ],
    "heal": [
        r"D:\mx\s41b3\logs\heal.log",
    ],
    "compat_deferral_source22": [
        r"D:\mx\s41b3\logs\compat_deferral_source22.log",
        r"D:\mx\s41b3\j74\summary.json",
    ],
    "compat_deferral_approved": [
        r"D:\mx\s41b3\logs\compat_deferral_approved.log",
        r"D:\mx\s41b3\j75\summary.json",
    ],
    "compat_extra_source22": [
        r"D:\mx\s41b3\logs\compat_extra_source22.log",
        r"D:\mx\s41b3\j80\summary.json",
        r"D:\mx\s41b3\j80\source22\runtime\LogConsole.txt",
    ],
    "compat_extra_approved": [
        r"D:\mx\s41b3\logs\compat_extra_approved.log",
        r"D:\mx\s41b3\j81\summary.json",
        r"D:\mx\s41b3\j81\approved\runtime\LogConsole.txt",
    ],
}

copied = []
for case, paths in wanted.items():
    cdir = OUT / "quotes" / case
    cdir.mkdir(parents=True, exist_ok=True)
    for p in paths:
        src = Path(p)
        dest = cdir / src.name
        if src.exists():
            dest.write_text(src.read_text(encoding="utf-8", errors="replace"), encoding="utf-8")
            copied.append(f"OK {src} -> {dest} {src.stat().st_size}")
        else:
            dest.write_text(f"MISSING {src}\n", encoding="utf-8")
            copied.append(f"MISSING {src}")
(OUT / "copied_quotes.txt").write_text("\n".join(copied), encoding="utf-8")
print("copied", len(copied))
