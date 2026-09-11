import json
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")

# fl200 delays in one-line JSON
for peer in ("host", "client"):
    p = Path(rf"D:\mx\s41b3\j28\fl\fl200\{peer}_report.json")
    data = json.loads(p.read_text(encoding="utf-8-sig"))
    ls = data["service"]["runner"]["lockstep"]
    (OUT / f"fl200_{peer}_delays.txt").write_text(
        json.dumps(
            {
                "path": str(p),
                "bytes": p.stat().st_size,
                "single_line": True,
                "peer_input_delays": ls.get("peer_input_delays"),
                "start_packets_sent": ls.get("start_packets_sent"),
                "start_retransmits": ls.get("start_retransmits"),
            },
            indent=2,
        ),
        encoding="utf-8",
    )

# S40 Rejected-a files
s40 = []
for p in [
    Path(r"D:\mx\s40lanes\h4\clean_leave_20260909_104531\ambiguous_host\runtime\LogConsole.txt"),
    Path(r"D:\mx\s40lanes\h4\reclaim_socket_20260909_104643\host\runtime\LogConsole.txt"),
    Path(r"D:\mx\s40lanes\h4\rejoin_after_resync_20260909_104844\resync_host\runtime\LogConsole.txt"),
]:
    lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(lines, 1):
        if "Rejected a" in line:
            s40.append(f"{p}:{i}:{line}")
(OUT / "s40_rejected_a.txt").write_text("\n".join(s40), encoding="utf-8")

# B1 host logs
for case, rel in (
    ("commit", r"D:\mx\s41b3\j43\substitute_commit_20260910_022955\host\stdout.log"),
    ("returner", r"D:\mx\s41b3\j44\substitute_returner_wins_20260910_023039\host\stdout.log"),
):
    p = Path(rel)
    text = p.read_text(encoding="utf-8", errors="replace") if p.exists() else f"MISSING {p}"
    (OUT / f"b1_{case}_host_stdout.txt").write_text(text, encoding="utf-8")

# fencing logs
for name, rel in (
    ("fencing_host_console", r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\host\runtime\LogConsole.txt"),
    ("fencing_client2_stdout", r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\client2\stdout.log"),
    ("fencing_host_stdout", r"D:\mx\s41b3\j36\fencing_two_transports_20260910_022107\host\stdout.log"),
):
    p = Path(rel)
    (OUT / f"{name}.txt").write_text(p.read_text(encoding="utf-8", errors="replace") if p.exists() else f"MISSING {p}", encoding="utf-8")

# pin
(OUT / "pin.json").write_text(Path(r"D:\mx\s41b3\pin.json").read_text(encoding="utf-8"), encoding="utf-8")

# extra approved first error
p = Path(r"D:\mx\s41b3\j81\approved\runtime\LogConsole.txt")
lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
hits = [f"{i}|{l}" for i, l in enumerate(lines, 1) if "HasAnySounds" in l]
(OUT / "extra_approved_hasanysounds.txt").write_text("\n".join(hits), encoding="utf-8")

print("s40", s40)
print("ok")
