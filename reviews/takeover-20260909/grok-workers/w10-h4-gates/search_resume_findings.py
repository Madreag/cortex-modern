"""Extract RESUME / plan passages about findings A/B and H4 gates."""
from __future__ import annotations

from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates")
NEEDLES = [
    "PumpSessionEvents",
    "TickAdmissionPlane",
    "15 ms",
    "finding A",
    "finding B",
    "seat hold",
    "silent connection",
    "activity ended in state Over",
    "Cannot save when there's no game running",
    "hosts_survived",
    "clean_old_ticket_refused",
    "used_stored_ticket",
    "host_fenced_the_old_transport",
    "host_bound_second_incarnation",
    "resync_match_continued",
    "02:20",
    "NetMatchService.cpp:724",
    "NetSession.cpp:165",
    "clean leave",
    "clean-leave",
    "P4 Alpha Duel",
    "setup_surface",
    "fixed-alpha-duel",
]

DOCS = [
    Path(r"D:\Projects\RESUME.md"),
    Path(r"D:\Projects\STAGE2_H4_RECONNECT_PLAN.md"),
]


def main() -> None:
    chunks = []
    for doc in DOCS:
        text = doc.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        chunks.append(f"===== {doc} lines={len(lines)} chars={len(text)} =====")
        for i, line in enumerate(lines, 1):
            low = line.lower()
            hits = [n for n in NEEDLES if n.lower() in low]
            if not hits:
                continue
            chunks.append(f"--- {doc.name}:{i} hits={hits} linelen={len(line)} ---")
            # print in 600-char windows around each hit
            for n in hits:
                idx = low.find(n.lower())
                start = max(0, idx - 200)
                end = min(len(line), idx + 400)
                chunks.append(f"  around '{n}' @{idx}:\n  {line[start:end]}\n")
    dest = OUT / "doc_findings.txt"
    dest.write_text("\n".join(chunks), encoding="utf-8")
    print("wrote", dest, dest.stat().st_size)


if __name__ == "__main__":
    main()
