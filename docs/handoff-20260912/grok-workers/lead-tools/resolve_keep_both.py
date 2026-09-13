"""Keep-both resolution on a conflicted WORKING file (auto-merged hunks stay): every conflict block becomes
ours + separator + theirs. Usage: resolve_keep_both.py <file> [--sep 'line1|line2'] [--theirs-first]"""
import pathlib
import sys

path = sys.argv[1]
sep = []
theirs_first = "--theirs-first" in sys.argv
if "--sep" in sys.argv:
    sep = sys.argv[sys.argv.index("--sep") + 1].split("|")
target = pathlib.Path(path)
text = target.read_text(encoding="utf-8", errors="surrogateescape")
crlf = "\r\n" in text
lines = text.replace("\r\n", "\n").split("\n")
out = []
i = 0
blocks = 0
while i < len(lines):
    if lines[i].startswith("<<<<<<< "):
        j = i + 1
        ours = []
        while not lines[j].startswith("======="):
            ours.append(lines[j]); j += 1
        j += 1
        theirs = []
        while not lines[j].startswith(">>>>>>> "):
            theirs.append(lines[j]); j += 1
        first, second = (theirs, ours) if theirs_first else (ours, theirs)
        out.extend(first)
        out.extend(sep)
        out.extend(second)
        blocks += 1
        i = j + 1
        continue
    out.append(lines[i])
    i += 1
result = "\n".join(out)
assert "<<<<<<<" not in result and ">>>>>>>" not in result
target.write_text(result.replace("\n", "\r\n") if crlf else result, encoding="utf-8", newline="")
print(path, "blocks resolved:", blocks, "crlf" if crlf else "lf")
