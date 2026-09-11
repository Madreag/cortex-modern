"""Extract numbered baseline lines for the 13 cases from report.md and related reviews."""
from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w2-breadth-triage")
report = Path(r"D:\Projects\reviews\claude-review-2026-09-08\lanes\source33-lanes\report.md")
lines = report.read_text(encoding="utf-8", errors="replace").splitlines()

# dump section headers with line numbers
headers = []
for i, line in enumerate(lines, 1):
    if line.startswith("## ") or line.startswith("### "):
        headers.append(f"{i}|{line}")
(OUT / "report_headers.txt").write_text("\n".join(headers), encoding="utf-8")

# extract Source38/39/40 bodies
def section(start_pat, end_pat):
    start = None
    for i, line in enumerate(lines):
        if start is None and start_pat in line and line.startswith("## "):
            start = i
        elif start is not None and line.startswith("## ") and end_pat in line:
            return start + 1, i, lines[start:i]
        elif start is not None and line.startswith("## ") and line != lines[start]:
            return start + 1, i, lines[start:i]
    if start is not None:
        return start + 1, len(lines), lines[start:]
    return None, None, []

for name, pat, end in (
    ("source38", "## Source38", "## Source39"),
    ("source39", "## Source39", "## Source40"),
    ("source40", "## Source40", "## Source41"),
):
    a, b, body = section(pat, end)
    numbered = [f"{a+i}|{line}" for i, line in enumerate(body)]
    (OUT / f"baseline_{name}.txt").write_text("\n".join(numbered), encoding="utf-8")
    print(name, a, b, "nlines", len(body))

# also extract key tables by keyword
needles = (
    "fl200",
    "clean_leave",
    "reclaim_socket",
    "fencing_two_transports",
    "rejoin_after_resync",
    "peers_3_4",
    "substitute",
    "heal",
    "compat",
    "spawn_child",
    "HasAnySounds",
    "auto delay",
    "auto-delay",
    "Rejected",
)
hits = []
for i, line in enumerate(lines, 1):
    if i < 1314:
        continue
    if any(n.lower() in line.lower() for n in needles):
        hits.append(f"{i}|{line}")
(OUT / "baseline_hits_s38plus.txt").write_text("\n".join(hits), encoding="utf-8")
print("hits", len(hits))
