"""Search long-line docs for H4 gate keywords. Writes only under w10-h4-gates."""
from __future__ import annotations

from pathlib import Path

OUT = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w10-h4-gates")
NEEDLES = (
    "clean_leave",
    "reclaim_socket",
    "fencing",
    "rejoin_after_resync",
    "AIOrder",
    "state Over",
)
DOCS = [
    Path(r"D:\Projects\RESUME.md"),
    Path(r"D:\Projects\STAGE2_H4_RECONNECT_PLAN.md"),
]


def main() -> None:
    chunks = []
    for doc in DOCS:
        chunks.append(f"===== {doc} exists={doc.exists()} =====")
        if not doc.exists():
            continue
        text = doc.read_text(encoding="utf-8", errors="replace")
        for i, line in enumerate(text.splitlines(), 1):
            hits = [n for n in NEEDLES if n.lower() in line.lower()]
            if not hits:
                continue
            shown = line[:600]
            chunks.append(f"{doc.name}:{i} hits={hits}\n{shown}\n")
    dest = OUT / "doc_hits.txt"
    dest.write_text("\n".join(chunks), encoding="utf-8")
    print("wrote", dest, "chars", dest.stat().st_size)


if __name__ == "__main__":
    main()
