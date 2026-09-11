import io

RULE = (
    "\n**Status reporting (binding, user's rule 2026-09-11).** `D:\\Projects\\STATUS.md` is the live status board. Every time the user "
    "asks what is being worked on or where things stand, the answer is that board: one line per §B-1 item 1-8 with its completion "
    "percentage, the change since the last time the user asked, and exactly what is being worked on right now (task and worker); "
    "nothing else, no phase names. The board is updated before answering and at every checkpoint, and each status request appends a "
    "history line, so the numbers stay consistent between asks.\n"
)

for name in ("AGENTS.md", "CLAUDE.md"):
    path = rf"D:\Projects\{name}"
    text = io.open(path, encoding="utf-8").read()
    marker = "## Mod compatibility (binding, 2026-09-08)"
    assert marker in text, name
    assert "Status reporting (binding" not in text, name
    text = text.replace(marker, RULE.lstrip("\n") + "\n" + marker, 1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)

a = io.open(r"D:\Projects\AGENTS.md", encoding="utf-8").read()
c = io.open(r"D:\Projects\CLAUDE.md", encoding="utf-8").read()
print("twins identical:", a == c)

P = r"D:\Projects\RESUME.md"
L = io.open(P, encoding="utf-8").read().split("\n")
i = next(k for k, l in enumerate(L) if l.startswith("# §B. WHERE WE ARE"))
L.insert(i + 1, "")
L.insert(i + 2, "**Live status board: `D:\\Projects\\STATUS.md`** (per-item % for §B-1 items 1-8, what is being worked on, change since the last ask; updated on every status request and checkpoint; user's rule 2026-09-11).")
io.open(P, "w", encoding="utf-8", newline="\n").write("\n".join(L))
print("resume pointer at line", i + 3)
